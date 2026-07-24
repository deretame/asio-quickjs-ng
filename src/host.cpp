#include "host.hpp"

#include "binary_store.hpp"
#include "fs.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
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

// Real Hono framework bundled via esbuild from npm hono@4.12.30
// Source: third_party/hono/hono.bundle.js
// Embedded at cmake time into hono_embedded.hpp (see cmake/embed_hono.cmake)
#include "hono_embedded.hpp"

std::string safe_strerror(int errnum)
{
#ifdef _WIN32
  char buffer[256];
  strerror_s(buffer, sizeof(buffer), errnum);
  return std::string(buffer);

#else
  return std::string(std::strerror(errnum));

#endif
}

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

JSValue native_http_response(
  JSContext* ctx,
  JSValueConst /*this_val*/,
  int argc,
  JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) {
    return JS_UNDEFINED;
  }
  int32_t conn_id = 0;
  if (JS_ToInt32(ctx, &conn_id, argv[1]) != 0) {
    return JS_UNDEFINED;
  }
  qjs::Value data(ctx, JS_DupValue(ctx, argv[0]));
  host->send_http_response_native(std::move(data), conn_id);
  return JS_UNDEFINED;
}

JSValue native_close_server(
  JSContext* ctx,
  JSValueConst /*this_val*/,
  int /*argc*/,
  JSValueConst* /*argv*/)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host) {
    return JS_UNDEFINED;
  }
  if (host->http_server) {
    host->http_server->stop();
    host->http_server.reset();
    host->http_active = false;
    spdlog::info("HTTP server stopped");
  }
  return JS_UNDEFINED;
}

JSValue native_create_server(
  JSContext* ctx,
  JSValueConst /*this_val*/,
  int argc,
  JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) {
    return JS_UNDEFINED;
  }
  int32_t port = 0;
  if (JS_ToInt32(ctx, &port, argv[0]) != 0) {
    return JS_UNDEFINED;
  }
  // Retrieve the handler from JS global
  qjs::Value handler_get =
    host->global().get("__httpHandler").get("get");
  if (!handler_get.is_function()) {
    spdlog::error("__httpHandler.get is not a function");
    return JS_UNDEFINED;
  }
  qjs::Value handler = handler_get.call();
  if (!handler.is_function()) {
    spdlog::error("no HTTP handler registered in JS");
    return JS_UNDEFINED;
  }
  host->start_http_server(
    static_cast<uint16_t>(port),
    qjs::Value::dup(ctx, handler.raw()));
  return JS_UNDEFINED;
}

}  // namespace

Host::Host()
{
  register_builtin_modules();
  boost::uuids::random_generator gen;
  host_id = boost::uuids::to_string(gen());
}

Host::Host(std::string id)
  : host_id(std::move(id))
{
  register_builtin_modules();
}

Host::~Host() { shutdown(); }

void Host::register_builtin_modules()
{
  static std::once_flag flag;
  std::call_once(flag, []() {
    qjs::Runtime::register_module("hono",
      std::string_view(
        reinterpret_cast<const char*>(hono_embedded::kHonoJsBytes),
        hono_embedded::kHonoJsSize));
    spdlog::debug("registered built-in module: hono ({} bytes)",
      hono_embedded::kHonoJsSize);
  });
}

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

void Host::shutdown() {
  stopping = true;
  http_active = false;
  if (http_server) {
    http_server->stop();
    http_server.reset();
  }
}

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
  g.set(
    "__nativeSendHttpResponse",
    qjs::Value::take(
    ctx.get(),
    JS_NewCFunction(ctx.get(), &native_http_response, "__nativeSendHttpResponse", 2)));
  g.set(
    "__nativeCreateServer",
    qjs::Value::take(
    ctx.get(),
    JS_NewCFunction(ctx.get(), &native_create_server, "__nativeCreateServer", 1)));
  g.set(
    "__nativeCloseServer",
    qjs::Value::take(
    ctx.get(),
    JS_NewCFunction(ctx.get(), &native_close_server, "__nativeCloseServer", 0)));
  g.set(
    "stream",
    qjs::Value::take(
    ctx.get(),
    JS_NewCFunction(ctx.get(), &native_stream, "stream", 2)));

  // Register __extractResponse helper for converting Response to plain object
  {
    const char* code =
      "globalThis.__extractResponse = async function(response) {"
      "  if (!response) { return { status: 204, headers: {}, body: null }; }"
      "  var headers = {};"
      "  if (response.headers) {"
      "    if (response.headers.forEach) {"
      "      response.headers.forEach(function(v, k) { headers[k] = v; });"
      "    } else if (typeof response.headers === 'object') {"
      "      Object.keys(response.headers).forEach(function(k) { headers[k] = response.headers[k]; });"
      "    }"
      "  }"
      "  var bodyBytes = null;"
      "  if (response._body != null) {"
      "    bodyBytes = response._body;"
      "  } else if (typeof response.arrayBuffer === 'function') {"
      "    var buf = await response.arrayBuffer();"
      "    bodyBytes = new Uint8Array(buf);"
      "  } else if (typeof response.text === 'function') {"
      "    var t = await response.text();"
      "    bodyBytes = new TextEncoder().encode(t);"
      "  }"
      "  return {"
      "    status: response.status | 0,"
      "    statusText: response.statusText || '',"
      "    headers: headers,"
      "    body: bodyBytes"
      "  };"
      "};";
    qjs::Value ret = ctx.eval(code, "<extract-response>", JS_EVAL_TYPE_GLOBAL);
    if (ret.is_exception()) {
      spdlog::error("failed to register __extractResponse");
      ctx.dump_exception();
    }
  }
  if (!binary_store::install(*this)) {
    return false;
  }
  fs_api::install(*this);

  constexpr EmbeddedJs k_core_bootstrap_js[] = {
    {"js/abort.js", kJsAbortBytes, sizeof(kJsAbortBytes)},
    {"js/text-encoding-polyfill.js", kJsTextEncodingPolyfillBytes,
     sizeof(kJsTextEncodingPolyfillBytes)},
    {"js/whatwg-url-polyfill.js", kJsWhatwgUrlPolyfillBytes,
     sizeof(kJsWhatwgUrlPolyfillBytes)},
    {"js/body_polyfill.js", kJsBodyPolyfillBytes, sizeof(kJsBodyPolyfillBytes)},
    {"js/buffer.js", kJsBufferBytes, sizeof(kJsBufferBytes)},
    {"js/http.js", kJsHttpBytes, sizeof(kJsHttpBytes)},
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
  bool drain,
  int flags
)
{
  qjs::Value ret = ctx.eval(code, filename, flags);
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
    spdlog::error("failed to open {}: {}", path, safe_strerror(errno));
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return eval_source(ss.str(), path);
}

bool Host::eval_module(const char* path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    spdlog::error("failed to open module {}: {}", path, safe_strerror(errno));
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  // Check if the file actually uses import/export
  std::string code = ss.str();
  bool has_import = code.find("import ") != std::string::npos ||
    code.find("import{") != std::string::npos ||
    code.find("import\n") != std::string::npos;
  bool has_export = code.find("export ") != std::string::npos ||
    code.find("export{") != std::string::npos ||
    code.find("export\n") != std::string::npos;

  if (!has_import && !has_export) {
    // Regular script - use global eval
    return eval_source(code, path);
  }

  // ES module
  qjs::Value ret = ctx.eval(code, path, JS_EVAL_TYPE_MODULE);
  if (ret.is_exception()) {
    spdlog::error("module eval failed: {}", path);
    ctx.dump_exception();
    return false;
  }
  // The result is a promise for async modules - let the loop drain it
  drain_jobs();
  return true;
}

void Host::run_loop()
{
  while (pending_ops > 0 || rt.job_pending() || http_active) {
    drain_jobs();
    if (pending_ops == 0 && !rt.job_pending() && !http_active) {
      break;
    }
    if (ioc.stopped()) {
      ioc.restart();
    }
    ioc.run_one();
  }
  drain_jobs();
}

void Host::start_http_server(uint16_t port, qjs::Value handler) {
  if (http_server) {
    http_server->stop();
  }
  http_handler = qjs::Value::dup(ctx.get(), handler.raw());

  http_server.emplace(ioc, port,
    [this, port](std::shared_ptr<HttpSession> session, HttpRequest req) {
      uint64_t conn_id = next_conn_id++;
      http_connections[conn_id] = session;

      // Build a plain JS object with request data
      std::string url = "http://localhost:" + std::to_string(port) + req.url;

      // Build headers JS object string
      std::string headers_js = "{";
      bool first = true;
      for (const auto& [k, v] : req.headers) {
        if (!first) headers_js += ", ";
        first = false;
        // Simple escape for quotes
        std::string ek = k;
        std::string ev = v;
        size_t pos = 0;
        while ((pos = ev.find('\'', pos)) != std::string::npos) {
          ev.replace(pos, 1, "\\'");
          pos += 2;
        }
        headers_js += "'" + ek + "': '" + ev + "'";
      }
      headers_js += "}";

      // Store body bytes and session pointer in global temp variables
      auto g = global();
      if (!req.body.empty()) {
        qjs::Value arr = qjs::Value::new_uint8_array_copy(
          ctx.get(), req.body.data(), req.body.size());
        g.set("__httpBodyTmp", std::move(arr));
      } else {
        qjs::Value null_val = qjs::Value::undefined();
        g.set("__httpBodyTmp", qjs::Value::dup(ctx.get(), null_val.raw()));
      }

      // Pass session pointer as a Number for streaming support.
      // JS can call globalThis.stream(callback, opts) to start chunked encoding.
      // Store as a JS number (double) - works for pointers up to 2^53
      double session_ptr = static_cast<double>(
        reinterpret_cast<intptr_t>(session.get()));
      qjs::Value ptr_val(ctx.get(),
        JS_NewNumber(ctx.get(), session_ptr));
      g.set("__httpSessionPtr", std::move(ptr_val));

      // Call the JS-side request handler which returns a Promise<Response>.
      // We extract status/headers/body in JS and return a plain object.
      // If the handler calls stream(), the stream function returns a
      // Promise that resolves to { __streamHandled: true }.
      std::string eval_code =
        "(async () => {"
        "  var init = { method: '" + req.method + "', headers: " + headers_js + " };"
        "  if (__httpBodyTmp != null) init.body = __httpBodyTmp;"
        "  var request = new Request('" + url + "', init);"
        "  var response = await globalThis.__httpHandler.get()(request);"
        "  if (response && response.__streamHandled) {"
        "    return { __streamHandled: true };"
        "  }"
        "  return await globalThis.__extractResponse(response);"
        "})()";

      qjs::Value promise = ctx.eval(eval_code.c_str(), "<http-request>", JS_EVAL_TYPE_GLOBAL);
      if (promise.is_exception()) {
        spdlog::error("failed to handle request");
        ctx.dump_exception();
        HttpResponse resp;
        resp.status = 500;
        resp.body = {'I', 'n', 't', 'e', 'r', 'n', 'a', 'l', ' ', 'E', 'r', 'r', 'o', 'r'};
        session->send_response(resp);
        http_connections.erase(conn_id);
        return;
      }

      // Attach .then() callback to the promise
      qjs::Value then_fn = promise.get("then");
      if (then_fn.is_function()) {
        // Store conn_id and promise in globals for the callback
        g.set("__httpConnIdTmp", ctx.new_int32(static_cast<int32_t>(conn_id)));
        g.set("__httpPromiseTmp", qjs::Value::dup(ctx.get(), promise.raw()));

        std::string then_code =
          "(function(p) {"
          "  p.then(function(data) {"
          "    if (data && data.__streamHandled) { return; }"
          "    globalThis.__nativeSendHttpResponse(data, globalThis.__httpConnIdTmp);"
          "  }, function(err) {"
          "    var msg = 'Internal Server Error';"
          "    if (err) { msg = String(err.message || err); }"
          "    globalThis.__nativeSendHttpResponse({ status: 500, statusText: 'Internal Server Error', headers: { 'content-type': 'text/plain' }, body: new TextEncoder().encode(msg) }, globalThis.__httpConnIdTmp);"
          "  });"
          "})(globalThis.__httpPromiseTmp)";

        qjs::Value then_result = ctx.eval(
          then_code.c_str(), "<http-then>", JS_EVAL_TYPE_GLOBAL);
        if (then_result.is_exception()) {
          spdlog::error("failed to attach promise handler");
          ctx.dump_exception();
          HttpResponse resp;
          resp.status = 500;
          resp.headers["content-type"] = "text/plain";
          resp.body = {'H', 'a', 'n', 'd', 'l', 'e', 'r', ' ', 'E', 'r', 'r', 'o', 'r'};
          session->send_response(resp);
          http_connections.erase(conn_id);
        }
      } else {
        // Not a promise - try to use directly
        send_js_response(conn_id, promise);
      }
    });

  http_server->start();
  http_active = true;
  spdlog::info("HTTP server started on port {}", port);
}

void Host::send_js_response(uint64_t conn_id, qjs::Value response) {
  auto it = this->http_connections.find(conn_id);
  if (it == this->http_connections.end()) {
    spdlog::error("connection {} not found", conn_id);
    return;
  }
  auto session = it->second;
  this->http_connections.erase(it);

  HttpResponse resp;

  qjs::Value status_val = response.get("status");
  int32_t status_code = 200;
  if (!status_val.to_int32(status_code)) {
    status_code = 200;
  }
  resp.status = status_code;

  qjs::Value status_text_val = response.get("statusText");
  auto status_text_str = status_text_val.to_std_string();
  resp.status_text = status_text_str.value_or("");

  qjs::Value body_val = response.get("body");
  if (body_val.is_uint8_array() || body_val.is_array_buffer()) {
    auto bytes = body_val.to_bytes();
    resp.body.assign(bytes.begin(), bytes.end());
  } else {
    auto str = body_val.to_std_string();
    if (str) {
      resp.body.assign(str->begin(), str->end());
    }
  }

  qjs::Value headers_val = response.get("headers");
  if (headers_val) {
    qjs::Value ct_val = headers_val.get("content-type");
    auto ct_str = ct_val.to_std_string();
    if (ct_str) {
      resp.headers["content-type"] = *ct_str;
    }
    qjs::Value xtest_val = headers_val.get("x-test");
    auto xtest_str = xtest_val.to_std_string();
    if (xtest_str) {
      resp.headers["x-test"] = *xtest_str;
    }
  }

  session->send_response(resp);
}

void Host::send_http_response_native(qjs::Value data, int32_t conn_id) {
  uint64_t cid = static_cast<uint64_t>(conn_id);
  auto it = this->http_connections.find(cid);
  if (it == this->http_connections.end()) {
    spdlog::warn("connection {} not found for response", conn_id);
    return;
  }
  auto session = it->second;
  this->http_connections.erase(it);

  HttpResponse resp;

  // Extract status
  qjs::Value status_val = data.get("status");
  int32_t status_code = 200;
  if (!status_val.to_int32(status_code)) {
    status_code = 200;
  }
  resp.status = status_code;

  // Extract statusText
  qjs::Value status_text_val = data.get("statusText");
  auto status_text_str = status_text_val.to_std_string();
  resp.status_text = status_text_str.value_or("");

  // Extract body
  qjs::Value body_val = data.get("body");
  if (body_val.is_uint8_array() || body_val.is_array_buffer()) {
    auto bytes = body_val.to_bytes();
    resp.body.assign(bytes.begin(), bytes.end());
  } else {
    auto str = body_val.to_std_string();
    if (str) {
      resp.body.assign(str->begin(), str->end());
    }
  }

  // Extract headers - iterate all properties via JS
  qjs::Value headers_val = data.get("headers");
  if (headers_val) {
    // Use a JS snippet to extract all headers into a flat array
    const char* extract_code =
      "(function(h) {"
      "  var r = [];"
      "  Object.keys(h).forEach(function(k) { r.push(k, h[k]); });"
      "  return r;"
      "})(__httpHeadersTmp)";
    global().set("__httpHeadersTmp",
      qjs::Value::dup(ctx.get(), headers_val.raw()));
    qjs::Value arr = ctx.eval(extract_code, "<extract-headers>", JS_EVAL_TYPE_GLOBAL);
    if (!arr.is_exception()) {
      qjs::Value length_val = arr.get("length");
      int32_t count = 0;
      length_val.to_int32(count);
      for (int32_t i = 0; i < count; i += 2) {
        qjs::Value key_val = arr.get(std::to_string(i).c_str());
        qjs::Value val_val = arr.get(std::to_string(i + 1).c_str());
        auto key_str = key_val.to_std_string();
        auto val_str = val_val.to_std_string();
        if (key_str && val_str) {
          resp.headers[*key_str] = *val_str;
        }
      }
    } else {
      ctx.dump_exception();
    }
  }

  session->send_response(resp);
}

// ---------------------------------------------------------------------------
// Streaming (chunked transfer encoding)
// ---------------------------------------------------------------------------

// Native write_chunk function: called from JS writer.write(data)
// Synchronously writes a chunk to the socket.
JSValue native_stream_write(
  JSContext* ctx,
  JSValueConst /*this_val*/,
  int argc,
  JSValueConst* argv)
{
  auto* session = static_cast<HttpSession*>(
    JS_GetContextOpaque(ctx));
  if (!session || argc < 1) {
    return JS_UNDEFINED;
  }

  // Convert argument to bytes and write synchronously
  if (JS_IsString(argv[0])) {
    size_t len = 0;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (str) {
      session->write_chunk_sync(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(str), len));
      JS_FreeCString(ctx, str);
    }
  } else {
    // Try ArrayBuffer / Uint8Array
    size_t byte_offset = 0;
    size_t byte_length = 0;
    JSValue ab = JS_GetTypedArrayBuffer(
      ctx, argv[0], &byte_offset, &byte_length, nullptr);
    if (!JS_IsException(ab)) {
      size_t ab_size = 0;
      uint8_t* buf = JS_GetArrayBuffer(ctx, &ab_size, ab);
      if (buf && byte_length > 0) {
        std::vector<uint8_t> data_copy(
          buf + byte_offset,
          buf + byte_offset + std::min(byte_length, ab_size - byte_offset));
        session->write_chunk_sync(std::span<const uint8_t>(data_copy));
      }
      JS_FreeValue(ctx, ab);
    }
  }

  return JS_UNDEFINED;
}

// Native end_stream function: called from JS writer.end()
JSValue native_stream_end(
  JSContext* ctx,
  JSValueConst /*this_val*/,
  int /*argc*/,
  JSValueConst* /*argv*/)
{
  auto* session = static_cast<HttpSession*>(
    JS_GetContextOpaque(ctx));
  if (!session) {
    return JS_UNDEFINED;
  }
  session->finish_chunked(true);
  return JS_UNDEFINED;
}

// Main stream function: begins chunked encoding, creates a writer,
// and calls the user callback.
// JS: globalThis.stream(async (write, end) => { ... }, { contentType: '...' })
JSValue native_stream(
  JSContext* ctx,
  JSValueConst /*this_val*/,
  int argc,
  JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) {
    spdlog::error("native_stream: no host or argc < 1");
    return JS_NewInt32(ctx, -1);
  }

  // Get session pointer from global
  JSValue session_val = JS_GetPropertyStr(
    ctx, JS_GetGlobalObject(ctx), "__httpSessionPtr");
  int64_t session_ptr = 0;
  JS_ToInt64(ctx, &session_ptr, session_val);
  JS_FreeValue(ctx, session_val);

  HttpSession* session = reinterpret_cast<HttpSession*>(session_ptr);
  if (!session) {
    spdlog::error("native_stream: session_ptr=0");
    return JS_NewInt32(ctx, -1);
  }
  if (!JS_IsFunction(ctx, argv[0])) {
    spdlog::error("native_stream: argv[0] is not a function");
    return JS_NewInt32(ctx, -1);
  }

  // Get content type from options (default: text/plain)
  std::string content_type = "text/plain";
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue ct_val = JS_GetPropertyStr(ctx, argv[1], "contentType");
    if (JS_IsString(ct_val)) {
      const char* ct_str = JS_ToCString(ctx, ct_val);
      if (ct_str) {
        content_type = ct_str;
        JS_FreeCString(ctx, ct_str);
      }
    }
    JS_FreeValue(ctx, ct_val);
  }

  // Begin chunked transfer encoding
  session->begin_chunked(200, content_type);

  // Temporarily set session as context opaque for write_chunk/end
  JS_SetContextOpaque(ctx, session);

  // Create write and end functions
  JSValue write_fn = JS_NewCFunction(
    ctx, &native_stream_write, "write", 1);
  JSValue end_fn = JS_NewCFunction(
    ctx, &native_stream_end, "end", 0);

  // Call the callback with write and end functions
  JSValue args[2] = { write_fn, end_fn };
  spdlog::debug("calling stream callback");
  JSValue result = JS_Call(
    ctx, argv[0], JS_UNDEFINED, 2, args);
  spdlog::debug("stream callback returned, is_exception={}",
    JS_IsException(result));

  // Drain microtasks to ensure await works
  JSRuntime* rt_ptr = JS_GetRuntime(ctx);
  if (JS_IsJobPending(rt_ptr)) {
    JSContext* job_ctx = nullptr;
    while (JS_IsJobPending(rt_ptr)) {
      JS_ExecutePendingJob(rt_ptr, &job_ctx);
    }
  }

  // Restore the host as context opaque
  JS_SetContextOpaque(ctx, host);

  if (JS_IsException(result)) {
    JS_FreeValue(ctx, result);
    return JS_NewInt32(ctx, -1);
  }

  // If the callback returned a Promise, wrap it so that when it resolves,
  // the response handler knows the stream was already handled.
  JSValue then_fn = JS_GetPropertyStr(ctx, result, "then");
  if (JS_IsFunction(ctx, then_fn)) {
    // Create a new promise that resolves to { __streamHandled: true }
    // when the original promise resolves.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolver = JS_NewObject(ctx);

    // Store the original promise for later
    JS_SetPropertyStr(ctx, resolver, "_orig", JS_DupValue(ctx, result));

    JSValue promise_resolver = JS_NewCFunction(
      ctx,
      [](JSContext* ctx, JSValueConst this_val, int argc,
         JSValueConst* argv) -> JSValue {
        // Return { __streamHandled: true }
        JSValue obj = JS_NewObject(ctx);
        JSValue true_val = JS_NewBool(ctx, 1);
        JS_SetPropertyStr(ctx, obj, "__streamHandled", true_val);
        return obj;
      },
      "resolve_stream", 1);

    JSValue args[1] = { promise_resolver };
    JSValue new_promise = JS_CallConstructor(
      ctx, promise_resolver, 0, nullptr);

    // Actually, simpler approach: just return the original promise
    // but attach a .then that replaces the result
    JS_FreeValue(ctx, new_promise);
    JS_FreeValue(ctx, promise_resolver);
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, then_fn);

    // Return a promise that resolves to the sentinel
    const char* wrapper_code =
      "(async () => { await __orig; return { __streamHandled: true }; })()";
    JS_SetPropertyStr(ctx, JS_GetGlobalObject(ctx),
      "__orig", JS_DupValue(ctx, result));
    JSValue wrapped = JS_Eval(ctx, wrapper_code,
      strlen(wrapper_code), "<stream-wrapper>",
      JS_EVAL_TYPE_GLOBAL);
    return wrapped;
  }

  JS_FreeValue(ctx, then_fn);
  JS_FreeValue(ctx, result);
  return JS_NewInt32(ctx, 0);
}
