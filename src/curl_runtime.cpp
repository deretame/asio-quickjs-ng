#include "curl_runtime.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>
#include <utility>

namespace curl_http {

struct CurlWatch : std::enable_shared_from_this<CurlWatch> {
  Runtime *runtime = nullptr;
  curl_socket_t fd = CURL_SOCKET_BAD;
  asio::ip::tcp::socket sock;
  int action = 0;
  bool read_armed = false;
  bool write_armed = false;

  explicit CurlWatch(Runtime *r, curl_socket_t s)
      : runtime(r), fd(s), sock(r->ioc_) {}

  bool alive() const {
    auto it = runtime->watches_.find(fd);
    return it != runtime->watches_.end() && it->second.get() == this;
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
    if (runtime->stopping_) {
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
      if (ec || self->runtime->stopping_) {
        return;
      }
      if (self->action & poll_bit) {
        self->runtime->on_socket_action(self->fd, ev_bit);
      }
      if (self->alive()) {
        self->arm();
      }
    });
  }
};

int curl_socket_callback(CURL * /*easy*/, curl_socket_t s, int what,
                         void *userp, void *socketp) {
  auto *runtime = static_cast<Runtime *>(userp);
  auto *old = static_cast<CurlWatch *>(socketp);

  if (what == CURL_POLL_REMOVE) {
    if (old) {
      old->dispose();
      runtime->multi_.assign(s, nullptr);
      runtime->watches_.erase(s);
    }
    return 0;
  }

  std::shared_ptr<CurlWatch> watch;
  if (old) {
    auto it = runtime->watches_.find(s);
    if (it == runtime->watches_.end()) {
      return -1;
    }
    watch = it->second;
  } else {
    watch = std::make_shared<CurlWatch>(runtime, s);
    if (!assign_socket(watch->sock, s)) {
      spdlog::error("assign curl socket failed");
      return -1;
    }
    runtime->watches_[s] = watch;
    runtime->multi_.assign(s, watch.get());
  }

  watch->update(what);
  return 0;
}

int curl_timer_callback(CURLM * /*multi*/, long timeout_ms, void *userp) {
  auto *runtime = static_cast<Runtime *>(userp);
  runtime->arm_timer(timeout_ms);
  return 0;
}

Runtime::Runtime(AsioExecutor &ex)
    : ex_(ex), ioc_(*static_cast<asio::io_context *>(ex.checkout())),
      timer_(ioc_) {
  multi_.set_socket_function(curl_socket_callback, this);
  multi_.set_timer_function(curl_timer_callback, this);
}

Runtime::~Runtime() { shutdown(); }

void Runtime::arm_timer(long timeout_ms) {
  timer_.cancel();
  if (timeout_ms < 0 || stopping_) {
    return;
  }
  timer_.expires_after(std::chrono::milliseconds(timeout_ms));
  timer_.async_wait([self = shared_from_this()](const asio::error_code &err) {
    if (err || self->stopping_) {
      return;
    }
    self->on_socket_action(CURL_SOCKET_TIMEOUT, 0);
  });
}

void Runtime::on_socket_action(curl_socket_t fd, int ev_bitmask) {
  if (!multi_ || stopping_) {
    return;
  }
  int running = 0;
  CURLMcode mc = multi_.socket_action(fd, ev_bitmask, &running);
  if (mc != CURLM_OK) {
    spdlog::error("curl_multi_socket_action: {}", curl_multi_strerror(mc));
  }
  on_multi_messages();
}

void Runtime::on_multi_messages() {
  int msgs = 0;
  while (CURLMsg *msg = multi_.info_read(&msgs)) {
    if (msg->msg != CURLMSG_DONE) {
      continue;
    }
    char *private_ptr = nullptr;
    curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &private_ptr);
    auto *tr = reinterpret_cast<Transfer *>(private_ptr);
    if (!tr) {
      continue;
    }
    if (!tr->finished) {
      auto result = tr->make_result(msg->data.result);
      tr->finish(std::move(result));
    }
    delete tr;
  }
}

bool Runtime::add_transfer(Transfer *tr) {
  if (!tr || !tr->easy) {
    return false;
  }
  tr->runtime = this;
  if (!tr->start()) {
    return false;
  }
  on_socket_action(CURL_SOCKET_TIMEOUT, 0);
  return true;
}

void Runtime::remove_transfer(Transfer *tr) {
  if (tr && tr->easy) {
    multi_.remove(tr->easy.get());
  }
}

void Runtime::notify() { on_socket_action(CURL_SOCKET_TIMEOUT, 0); }

void Runtime::shutdown() {
  stopping_ = true;
  timer_.cancel();
  for (auto &kv : watches_) {
    kv.second->dispose();
  }
  watches_.clear();
}

// Transfer method implementations (moved here because they need Runtime).

size_t Transfer::write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *self = static_cast<Transfer *>(userdata);
  self->body.append(ptr, size * nmemb);
  return size * nmemb;
}

size_t Transfer::header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *self = static_cast<Transfer *>(userdata);
  const size_t n = size * nmemb;
  std::string line(ptr, n);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  if (line.empty()) {
    return n;
  }
  if (line.rfind("HTTP/", 0) == 0) {
    self->response_headers.clear();
    self->status_text.clear();
    auto sp1 = line.find(' ');
    if (sp1 != std::string::npos) {
      auto sp2 = line.find(' ', sp1 + 1);
      if (sp2 != std::string::npos && sp2 + 1 < line.size()) {
        self->status_text = line.substr(sp2 + 1);
      }
    }
    return n;
  }
  auto colon = line.find(':');
  if (colon == std::string::npos) {
    return n;
  }
  HeaderPair hp;
  hp.name = line.substr(0, colon);
  hp.value = line.substr(colon + 1);
  while (!hp.value.empty() &&
         (hp.value.front() == ' ' || hp.value.front() == '\t')) {
    hp.value.erase(hp.value.begin());
  }
  self->response_headers.push_back(std::move(hp));
  return n;
}

void Transfer::finish(FetchResult result) {
  if (finished) {
    return;
  }
  finished = true;
  auto done = std::move(complete);
  if (easy && runtime) {
    easy.setopt(CURLOPT_PRIVATE, nullptr);
    runtime->remove_transfer(this);
    easy.reset();
  }
  if (req_headers) {
    curl_slist_free_all(req_headers);
    req_headers = nullptr;
  }
  if (done) {
    done(std::move(result));
  }
}

void Transfer::abort() {
  FetchResult r;
  r.ok = false;
  r.aborted = true;
  r.error = "aborted";
  r.url = options.url;
  finish(std::move(r));
}

bool Transfer::start() {
  if (!easy || !runtime) {
    return false;
  }
  easy.setopt(CURLOPT_URL, options.url.c_str());
  easy.setopt(CURLOPT_FOLLOWLOCATION, options.follow_redirects ? 1L : 0L);
  easy.setopt(CURLOPT_MAXREDIRS, 20L);
  easy.setopt(CURLOPT_WRITEFUNCTION, &Transfer::write_cb);
  easy.setopt(CURLOPT_WRITEDATA, this);
  easy.setopt(CURLOPT_HEADERFUNCTION, &Transfer::header_cb);
  easy.setopt(CURLOPT_HEADERDATA, this);
  easy.setopt(CURLOPT_PRIVATE, this);
  easy.setopt(CURLOPT_ERRORBUFFER, errbuf);
  easy.setopt(CURLOPT_USERAGENT, "asio-quickjs-ng/0.1");
  easy.setopt(CURLOPT_ACCEPT_ENCODING, "");
  easy.setopt(CURLOPT_TIMEOUT, options.timeout_sec);
#ifdef CURLSSLOPT_NATIVE_CA
  easy.setopt(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif

  const std::string method = options.method.empty() ? "GET" : options.method;
  if (method == "GET") {
    easy.setopt(CURLOPT_HTTPGET, 1L);
  } else if (method == "HEAD") {
    easy.setopt(CURLOPT_NOBODY, 1L);
  } else if (method == "POST") {
    easy.setopt(CURLOPT_POST, 1L);
    easy.setopt(CURLOPT_POSTFIELDS, options.body.c_str());
    easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(options.body.size()));
  } else if (method == "PUT") {
    easy.setopt(CURLOPT_CUSTOMREQUEST, "PUT");
    easy.setopt(CURLOPT_POSTFIELDS, options.body.c_str());
    easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(options.body.size()));
  } else {
    easy.setopt(CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!options.body.empty()) {
      easy.setopt(CURLOPT_POSTFIELDS, options.body.c_str());
      easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE,
                  static_cast<curl_off_t>(options.body.size()));
    }
  }

  for (const auto &h : options.headers) {
    std::string line = h.name + ": " + h.value;
    req_headers = curl_slist_append(req_headers, line.c_str());
  }
  if (req_headers) {
    easy.setopt(CURLOPT_HTTPHEADER, req_headers);
  }

  return runtime->multi_.add(easy.get()) == CURLM_OK;
}

FetchResult Transfer::make_result(CURLcode code) {
  FetchResult r;
  r.url = options.url;
  char *effective = nullptr;
  if (easy.getinfo(CURLINFO_EFFECTIVE_URL, &effective) && effective) {
    r.url = effective;
  }
  easy.getinfo(CURLINFO_RESPONSE_CODE, &r.status);
  long redirect_count = 0;
  easy.getinfo(CURLINFO_REDIRECT_COUNT, &redirect_count);
  r.redirected = redirect_count > 0;
  r.status_text = status_text;
  r.headers = std::move(response_headers);
  if (code == CURLE_OK) {
    if (options.fail_on_redirect && r.status >= 300 && r.status < 400) {
      r.ok = false;
      r.error = "redirect not allowed";
      r.body = std::move(body);
    } else {
      r.ok = true;
      r.body = std::move(body);
    }
  } else {
    r.ok = false;
    r.error = errbuf[0] ? errbuf : curl_easy_strerror(code);
  }
  return r;
}

} // namespace curl_http
