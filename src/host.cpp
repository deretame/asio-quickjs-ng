#include "host.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "function_registry.hpp"
#include "js_embedded.hpp"

namespace {

std::string join_args(qjs::Args args)
{
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

void log_args(spdlog::level::level_enum level, qjs::Args args)
{
  spdlog::log(level, "{}", join_args(args));
}

void print_fn(qjs::Args args) { log_args(spdlog::level::info, args); }

void console_debug_fn(qjs::Args args) { log_args(spdlog::level::debug, args); }

void console_info_fn(qjs::Args args) { log_args(spdlog::level::info, args); }

void console_log_fn(qjs::Args args) { log_args(spdlog::level::info, args); }

void console_warn_fn(qjs::Args args) { log_args(spdlog::level::warn, args); }

void console_error_fn(qjs::Args args) { log_args(spdlog::level::err, args); }

async_simple::coro::Lazy<void> timeout_coro(
  Host* host,
  qjs::Value callback,
  int32_t id,
  std::shared_ptr<asio::steady_timer> timer,
  std::chrono::milliseconds delay
)
{
  async_simple::Promise<void> p;
  auto fut = p.getFuture();
  timer->expires_after(delay);
  timer->async_wait(
    [p = std::move(p), timer](const asio::error_code&) mutable {
      p.setValue();
    });
  co_await std::move(fut);

  if (!host->stopping && host->erase_timer_if_active(id)) {
    qjs::Value ret = callback.call();
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
    host->drain_jobs();
  }
  --host->pending_ops;
  co_return;
}

async_simple::coro::Lazy<void> interval_coro(
  Host* host,
  qjs::Value callback,
  int32_t id,
  std::shared_ptr<asio::steady_timer> timer,
  std::chrono::milliseconds delay
)
{
  while (true) {
    async_simple::Promise<void> p;
    auto fut = p.getFuture();
    timer->expires_after(delay);
    timer->async_wait(
      [p = std::move(p), timer](const asio::error_code&) mutable {
        p.setValue();
      });
    co_await std::move(fut);

    if (host->stopping || !host->timer_is_active(id)) {
      break;
    }

    qjs::Value ret = callback.call();
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
    host->drain_jobs();

    if (!host->timer_is_active(id)) {
      break;
    }
  }
  --host->pending_ops;
  co_return;
}

int32_t set_interval_fn(
  Host* host,
  qjs::Value callback,
  std::optional<int32_t> delay_ms
)
{
  if (!callback.is_function()) {
    host->throw_type_error("setInterval(fn, ms)");
  }
  int32_t ms = delay_ms.value_or(0);
  if (ms < 0) {
    ms = 0;
  }
  auto timer = std::make_shared<asio::steady_timer>(host->ioc);
  int32_t id = host->register_timer(timer);
  ++host->pending_ops;
  host->spawn_lazy(
    interval_coro(
    host,
    std::move(callback),
    id,
    std::move(timer),
    std::chrono::milliseconds(ms)));
  return id;
}

void clear_interval_fn(Host* host, int32_t id)
{
  host->cancel_timer(id);
}

int32_t set_timeout_fn(
  Host* host,
  qjs::Value callback,
  std::optional<int32_t> delay_ms
)
{
  if (!callback.is_function()) {
    host->throw_type_error("setTimeout(fn, ms)");
  }
  int32_t ms = delay_ms.value_or(0);
  if (ms < 0) {
    ms = 0;
  }
  auto timer = std::make_shared<asio::steady_timer>(host->ioc);
  int32_t id = host->register_timer(timer);
  ++host->pending_ops;
  host->spawn_lazy(
    timeout_coro(
    host,
    std::move(callback),
    id,
    std::move(timer),
    std::chrono::milliseconds(ms)));
  return id;
}

void clear_timeout_fn(Host* host, int32_t id)
{
  host->cancel_timer(id);
}

std::string base64_encode_bytes(const uint8_t* data, size_t len)
{
  if (len == 0) {
    return {};
  }
  size_t encoded_len = ((len + 2) / 3) * 4;
  std::string out(encoded_len + 1, '\0');
  int actual_len = EVP_EncodeBlock(
    reinterpret_cast<unsigned char*>(out.data()),
    data,
    static_cast<int>(len));
  if (actual_len < 0) {
    throw std::runtime_error("base64 encode failed");
  }
  out.resize(static_cast<size_t>(actual_len));
  return out;
}

std::vector<uint8_t> base64_decode_string(std::string_view s)
{
  if (s.empty()) {
    return {};
  }
  size_t max_len = (s.size() + 3) / 4 * 3;
  std::vector<uint8_t> out(max_len);
  int len = EVP_DecodeBlock(
    out.data(),
    reinterpret_cast<const unsigned char*>(s.data()),
    static_cast<int>(s.size()));
  if (len < 0) {
    throw std::runtime_error("base64 decode failed");
  }
  size_t padding = 0;
  if (!s.empty() && s[s.size() - 1] == '=') {
    ++padding;
    if (s.size() >= 2 && s[s.size() - 2] == '=') {
      ++padding;
    }
  }
  out.resize(static_cast<size_t>(len) - padding);
  return out;
}

qjs::Value base64_encode_fn(Host* host, qjs::Value input)
{
  std::string out;
  if (input.is_binary_view()) {
    // Zero-copy path for binary input.
    auto bytes = input.to_bytes();
    out = base64_encode_bytes(bytes.data(), bytes.size());
  } else if (input.is_string()) {
    auto s = input.to_std_string();
    if (!s) {
      host->throw_type_error(
        "base64.encode(data): failed to read string");
    }
    out = base64_encode_bytes(
      reinterpret_cast<const uint8_t*>(s->data()),
      s->size());
  } else {
    host->throw_type_error(
      "base64.encode(data): data must be string, ArrayBuffer, or Uint8Array");
  }
  return host->ctx.new_string(out);
}

std::vector<uint8_t> base64_decode_fn(Host* host, std::string input)
{
  return base64_decode_string(input);
}

}  // namespace

Host::Host()
{
  boost::uuids::random_generator gen;
  host_id = boost::uuids::to_string(gen());
}

Host::Host(std::string id) : host_id(std::move(id)) {}

Host::~Host() { shutdown(); }

int32_t Host::register_timer(std::shared_ptr<asio::steady_timer> timer)
{
  int32_t id = next_timer_id_++;
  active_timers_[id] = std::move(timer);
  return id;
}

void Host::cancel_timer(int32_t id)
{
  auto it = active_timers_.find(id);
  if (it == active_timers_.end()) {
    return;
  }
  auto timer = std::move(it->second);
  active_timers_.erase(it);
  if (timer) {
    timer->cancel();
  }
}

void Host::cancel_all_timers()
{
  std::vector<int32_t> ids;
  ids.reserve(active_timers_.size());
  for (const auto& [id, _] : active_timers_) {
    ids.push_back(id);
  }
  for (int32_t id : ids) {
    cancel_timer(id);
  }
}

bool Host::erase_timer_if_active(int32_t id)
{
  auto it = active_timers_.find(id);
  if (it == active_timers_.end()) {
    return false;
  }
  active_timers_.erase(it);
  return true;
}

bool Host::timer_is_active(int32_t id) const
{
  return active_timers_.find(id) != active_timers_.end();
}

void Host::spdlog_lazy_error(const char* msg)
{
  spdlog::error("lazy exception: {}", msg);
}

void Host::register_function(const std::string& name, SyncFunction fn)
{
  registry.register_function(name, std::move(fn));
}

void Host::register_async_function(const std::string& name, AsyncFunction fn)
{
  registry.register_async_function(name, std::move(fn));
}

void Host::register_global_function(const std::string& name, SyncFunction fn)
{
  FunctionRegistry::register_global_function(name, std::move(fn));
}

void Host::register_global_async_function(
  const std::string& name,
  AsyncFunction fn
)
{
  FunctionRegistry::register_global_async_function(name, std::move(fn));
}

void Host::shutdown() { stopping = true; }

void Host::throw_type_error(const char* msg)
{
  JS_ThrowTypeError(ctx.get(), "%s", msg);
  throw std::runtime_error(msg);
}

void Host::throw_internal_error(const char* msg)
{
  JS_ThrowInternalError(ctx.get(), "%s", msg);
  throw std::runtime_error(msg);
}

async_simple::coro::Lazy<void> Host::async_sleep(std::chrono::milliseconds ms)
{
  async_simple::Promise<void> p;
  auto fut = p.getFuture();
  auto timer = std::make_shared<asio::steady_timer>(ioc);
  timer->expires_after(ms);
  timer->async_wait(
    [p = std::move(p), timer](const asio::error_code&) mutable {
      p.setValue();
    });
  co_await std::move(fut);
  co_return;
}

bool Host::install_runtime()
{
  using namespace js_embedded;

  ctx.set_opaque(this);
  auto g = global();
  g.fn<&print_fn>("print");
  g.obj(
    "base64",
    [](qjs::Value& b) {
      b.fn<&base64_encode_fn>("encode");
      b.fn<&base64_decode_fn>("decode");
    });
  g.fn<&set_timeout_fn>("setTimeout");
  g.fn<&clear_timeout_fn>("clearTimeout");
  g.fn<&set_interval_fn>("setInterval");
  g.fn<&clear_interval_fn>("clearInterval");
  g.obj(
    "console",
    [](qjs::Value& c) {
      c.fn<&console_log_fn>("log");
      c.fn<&console_debug_fn>("debug");
      c.fn<&console_info_fn>("info");
      c.fn<&console_warn_fn>("warn");
      c.fn<&console_error_fn>("error");
    });
  g.set("__hostID", ctx.new_string(host_id));
  g.set(
    "call",
    qjs::Value::take(
    ctx.get(),
    JS_NewCFunction(ctx.get(), &native_call, "call", 1)));

  constexpr EmbeddedJs k_core_bootstrap_js[] = {
    {"js/abort.js", kJsAbortBytes, sizeof(kJsAbortBytes)},
    {"js/text-encoding-polyfill.js", kJsTextEncodingPolyfillBytes,
     sizeof(kJsTextEncodingPolyfillBytes)},
    {"js/whatwg-url-polyfill.js", kJsWhatwgUrlPolyfillBytes,
     sizeof(kJsWhatwgUrlPolyfillBytes)},
    {"js/body_polyfill.js", kJsBodyPolyfillBytes, sizeof(kJsBodyPolyfillBytes)},
    {"js/buffer.js", kJsBufferBytes, sizeof(kJsBufferBytes)},
  };
  return install_bootstrap_js(k_core_bootstrap_js);
}

bool Host::install_bootstrap_js(std::span<const EmbeddedJs> scripts)
{
  for (const EmbeddedJs& script : scripts) {
    std::string src{reinterpret_cast<const char*>(script.bytes), script.size};
    qjs::Value ret = ctx.eval(src, script.name, JS_EVAL_TYPE_GLOBAL);
    if (ret.is_exception()) {
      spdlog::error("bootstrap failed: {}", script.name);
      ctx.dump_exception();
      return false;
    }
  }
  drain_jobs();
  return true;
}

bool Host::eval_source(
  std::string_view code,
  const char* filename,
  bool drain
)
{
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

bool Host::eval_file(const char* path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    spdlog::error("failed to open {}: {}", path, std::strerror(errno));
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return eval_source(ss.str(), path);
}

void Host::run_loop()
{
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
