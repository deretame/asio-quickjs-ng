#include "function_registry.hpp"
#include "host.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

void on_socket_action(Host *host, curl_socket_t fd, int ev_bitmask);
void on_multi_messages(Host *host);

} // namespace

struct CurlWatch : std::enable_shared_from_this<CurlWatch> {
  Host *host = nullptr;
  curl_socket_t fd = CURL_SOCKET_BAD;
  asio::ip::tcp::socket sock;
  int action = 0;
  bool read_armed = false;
  bool write_armed = false;

  explicit CurlWatch(Host *h, curl_socket_t s)
      : host(h), fd(s), sock(h->ioc) {}

  bool alive() const {
    auto it = host->watches.find(fd);
    return it != host->watches.end() && it->second.get() == this;
  }

  void update(int what) {
    action = what;
    arm();
  }

  void dispose() {
    asio::error_code ec;
    action = 0;
    sock.cancel();
    sock.release(ec);
  }

  void arm() {
    if (host->stopping) {
      return;
    }
    arm_dir(CURL_POLL_IN, CURL_CSELECT_IN, read_armed,
            asio::ip::tcp::socket::wait_read);
    arm_dir(CURL_POLL_OUT, CURL_CSELECT_OUT, write_armed,
            asio::ip::tcp::socket::wait_write);
  }

private:
  void arm_dir(int poll_bit, int ev_bit, bool & /*armed*/,
               asio::ip::tcp::socket::wait_type wait) {
    bool &flag = (poll_bit == CURL_POLL_IN) ? read_armed : write_armed;
    if (!(action & poll_bit) || flag) {
      return;
    }
    flag = true;
    auto self = shared_from_this();
    sock.async_wait(wait, [self, poll_bit, ev_bit](const asio::error_code &ec) {
      bool &armed =
          (poll_bit == CURL_POLL_IN) ? self->read_armed : self->write_armed;
      armed = false;
      if (ec || self->host->stopping) {
        return;
      }
      if (self->action & poll_bit) {
        on_socket_action(self->host, self->fd, ev_bit);
      }
      if (self->alive()) {
        self->arm();
      }
    });
  }
};

namespace {

void on_socket_action(Host *host, curl_socket_t fd, int ev_bitmask) {
  if (!host->multi || host->stopping) {
    return;
  }
  int running = 0;
  CURLMcode mc = host->multi.socket_action(fd, ev_bitmask, &running);
  if (mc != CURLM_OK) {
    spdlog::error("curl_multi_socket_action: {}", curl_multi_strerror(mc));
  }
  on_multi_messages(host);
}

int curl_socket_cb(CURL * /*easy*/, curl_socket_t s, int what, void *userp,
                   void *socketp) {
  auto *host = static_cast<Host *>(userp);
  auto *old = static_cast<CurlWatch *>(socketp);

  if (what == CURL_POLL_REMOVE) {
    if (old) {
      old->dispose();
      host->multi.assign(s, nullptr);
      host->watches.erase(s);
    }
    return 0;
  }

  std::shared_ptr<CurlWatch> watch;
  if (old) {
    auto it = host->watches.find(s);
    if (it == host->watches.end()) {
      return -1;
    }
    watch = it->second;
  } else {
    watch = std::make_shared<CurlWatch>(host, s);
    if (!curl_http::assign_socket(watch->sock, s)) {
      spdlog::error("assign curl socket failed");
      return -1;
    }
    host->watches[s] = watch;
    host->multi.assign(s, watch.get());
  }

  watch->update(what);
  return 0;
}

int curl_timer_cb(CURLM * /*multi*/, long timeout_ms, void *userp) {
  auto *host = static_cast<Host *>(userp);
  host->multi_timer.cancel();
  if (timeout_ms < 0 || host->stopping) {
    return 0;
  }
  host->multi_timer.expires_after(std::chrono::milliseconds(timeout_ms));
  host->multi_timer.async_wait([host](const asio::error_code &err) {
    if (err || host->stopping) {
      return;
    }
    on_socket_action(host, CURL_SOCKET_TIMEOUT, 0);
  });
  return 0;
}

void on_multi_messages(Host *host) {
  int msgs = 0;
  while (CURLMsg *msg = host->multi.info_read(&msgs)) {
    if (msg->msg != CURLMSG_DONE) {
      continue;
    }
    char *private_ptr = nullptr;
    curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &private_ptr);
    auto *tr = reinterpret_cast<curl_http::Transfer *>(private_ptr);
    if (!tr) {
      continue;
    }
    if (tr->id != 0) {
      host->fetch_transfers.erase(tr->id);
    }
    if (!tr->finished) {
      auto result = tr->make_result(msg->data.result);
      tr->finish(std::move(result));
    }
    delete tr;
  }
}

std::string join_args(qjs::Args args) {
  std::string out;
  for (int i = 0; i < args.size(); ++i) {
    if (i > 0) {
      out.push_back(' ');
    }
    auto s = args[i].to_std_string();
    if (!s) {
      throw qjs::detail::ConvertError{};
    }
    out += *s;
  }
  return out;
}

void log_args(spdlog::level::level_enum level, qjs::Args args) {
  spdlog::log(level, "{}", join_args(args));
}

void print_fn(qjs::Args args) { log_args(spdlog::level::info, args); }
void console_debug_fn(qjs::Args args) { log_args(spdlog::level::debug, args); }
void console_info_fn(qjs::Args args) { log_args(spdlog::level::info, args); }
void console_log_fn(qjs::Args args) { log_args(spdlog::level::info, args); }
void console_warn_fn(qjs::Args args) { log_args(spdlog::level::warn, args); }
void console_error_fn(qjs::Args args) { log_args(spdlog::level::err, args); }

async_simple::coro::Lazy<void>
timeout_coro(Host *host, qjs::Value callback, std::chrono::milliseconds delay) {
  co_await host->async_sleep(delay);
  if (!host->stopping) {
    qjs::Value ret = callback.call();
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
    host->drain_jobs();
  }
  --host->pending_ops;
  co_return;
}

void set_timeout_fn(Host *host, qjs::Value callback,
                    std::optional<int32_t> delay_ms) {
  if (!callback.is_function()) {
    host->throw_type_error("setTimeout(fn, ms)");
  }
  int32_t ms = delay_ms.value_or(0);
  if (ms < 0) {
    ms = 0;
  }
  ++host->pending_ops;
  host->spawn_lazy(timeout_coro(host, std::move(callback),
                                std::chrono::milliseconds(ms)));
}

} // namespace

Host::Host() {
  std::random_device rd;
  std::mt19937 rng(rd());
  uuids::uuid_random_generator gen(rng);
  host_id = uuids::to_string(gen());
}

Host::Host(std::string id) : host_id(std::move(id)) {}

Host::~Host() { shutdown(); }

void Host::spdlog_lazy_error(const char *msg) {
  spdlog::error("lazy exception: {}", msg);
}

void Host::register_function(const std::string &name, SyncFunction fn) {
  registry.register_function(name, std::move(fn));
}

void Host::register_async_function(const std::string &name,
                                   AsyncFunction fn) {
  registry.register_async_function(name, std::move(fn));
}

void Host::bind_curl() {
  multi.set_socket_function(curl_socket_cb, this);
  multi.set_timer_function(curl_timer_cb, this);
}

void Host::shutdown() {
  stopping = true;
  multi_timer.cancel();
  for (auto &kv : watches) {
    kv.second->dispose();
  }
  watches.clear();
}

void Host::notify_curl() { on_socket_action(this, CURL_SOCKET_TIMEOUT, 0); }

void Host::throw_type_error(const char *msg) {
  JS_ThrowTypeError(ctx.get(), "%s", msg);
  throw std::runtime_error(msg);
}

void Host::throw_internal_error(const char *msg) {
  JS_ThrowInternalError(ctx.get(), "%s", msg);
  throw std::runtime_error(msg);
}

async_simple::coro::Lazy<void>
Host::async_sleep(std::chrono::milliseconds ms) {
  async_simple::Promise<void> p;
  auto fut = p.getFuture();
  auto timer = std::make_shared<asio::steady_timer>(ioc);
  timer->expires_after(ms);
  timer->async_wait(
      [p = std::move(p), timer](const asio::error_code &) mutable {
        p.setValue();
      });
  co_await std::move(fut);
  co_return;
}

bool Host::install_runtime() {
  ctx.set_opaque(this);
  auto g = global();
  g.fn<&print_fn>("print");
  g.fn<&set_timeout_fn>("setTimeout");
  g.obj("console", [](qjs::Value &c) {
    c.fn<&console_log_fn>("log");
    c.fn<&console_debug_fn>("debug");
    c.fn<&console_info_fn>("info");
    c.fn<&console_warn_fn>("warn");
    c.fn<&console_error_fn>("error");
  });
  g.set("__hostID", ctx.new_string(host_id));
  g.set("call",
        qjs::Value::take(ctx.get(),
                         JS_NewCFunction(ctx.get(), &native_call, "call", 1)));
  return true;
}

bool Host::eval_source(std::string_view code, const char *filename,
                       bool drain) {
  qjs::Value ret = ctx.eval(code, filename, JS_EVAL_TYPE_GLOBAL);
  if (ret.is_exception()) {
    ctx.dump_exception();
    return false;
  }
  if (drain) {
    drain_jobs();
  }
  return true;
}

bool Host::eval_file(const char *path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    spdlog::error("failed to open {}: {}", path, std::strerror(errno));
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return eval_source(ss.str(), path);
}

void Host::run_loop() {
  while (pending_ops > 0 || rt.job_pending()) {
    drain_jobs();
    if (pending_ops == 0 && !rt.job_pending()) {
      break;
    }
    if (ioc.stopped()) {
      ioc.restart();
    }
    ioc.run_one();
  }
  drain_jobs();
}
