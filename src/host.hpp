#pragma once

#include <async_simple/Promise.h>
#include <async_simple/Try.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <span>
#include <string>
#include <unordered_map>
#include <functional>

#include "net/http_server.hpp"
#include "qjs.hpp"
#include "function_registry.hpp"
#include "asio_executor.hpp"
#include "curl_http.hpp"

// Forward declarations for streaming functions (defined in host.cpp)
JSValue native_stream(
  JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue native_stream_write(
  JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue native_stream_end(
  JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

struct Host {
  asio::io_context ioc{1};
  AsioExecutor ex{ioc};
  qjs::Runtime rt;
  qjs::Context ctx{rt};
  int pending_ops = 0;
  bool stopping = false;

  // In-flight fetch transfers (id -> Transfer*), for AbortSignal cancellation.
  uint64_t next_fetch_id = 1;
  std::unordered_map<uint64_t, curl_http::Transfer*> fetch_transfers;

  // Dynamic function registry: call(name, ...args) from JS.
  FunctionRegistry registry;

  // Per-instance ID. Default is a UUID v4 generated at construction.
  std::string host_id;

  // HTTP server
  std::optional<HttpServer> http_server;
  bool http_active = false;
  qjs::Value http_handler;
  uint64_t next_conn_id = 1;
  std::unordered_map<uint64_t, std::shared_ptr<HttpSession>> http_connections;

  Host();
  explicit Host(std::string id);
  ~Host();

  // Register built-in modules (hono, etc.) in the JS runtime module loader.
  static void register_builtin_modules();

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  explicit operator bool() const {
    return rt && ctx;
  }

  void shutdown();
  void run_loop();
  void drain_jobs() { rt.drain_jobs(); }

  qjs::Value global() { return ctx.global(); }

  qjs::Ctx js() { return ctx.ref(); }

  JSContext* js_raw() { return ctx.get(); }

  const std::string& id() const { return host_id; }

  template <auto Fn>
  qjs::Value func(const char* name)
  {
    return ctx.func<Fn>(name);
  }

  void register_function(const std::string& name, SyncFunction fn);
  void register_async_function(const std::string& name, AsyncFunction fn);

  // Global registration: functions registered here are visible to all Host
  // instances, even if registered while a Host is already running.
  static void register_global_function(
    const std::string& name,
    SyncFunction fn
    );
  static void register_global_async_function(
    const std::string& name,
    AsyncFunction fn
    );

  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, SyncFunction>)
  void register_function(const std::string& name, Fn&& fn)
  {
    registry.register_function(name, std::forward<Fn>(fn));
  }

  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, AsyncFunction>)
  void register_async_function(const std::string& name, Fn&& fn)
  {
    registry.register_async_function(name, std::forward<Fn>(fn));
  }

  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, SyncFunction>)
  static void register_global_function(const std::string& name, Fn&& fn)
  {
    FunctionRegistry::register_global_function(name, std::forward<Fn>(fn));
  }

  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, AsyncFunction>)
  static void register_global_async_function(
    const std::string& name,
    Fn&& fn
    )
  {
    FunctionRegistry::register_global_async_function(
      name,
      std::forward<Fn>(fn));
  }

  // Timer registry for setTimeout / setInterval / clearTimeout / clearInterval.
  int32_t register_timer(std::shared_ptr<asio::steady_timer> timer);
  void cancel_timer(int32_t id);
  void cancel_all_timers();
  bool erase_timer_if_active(int32_t id);
  bool timer_is_active(int32_t id) const;

  [[noreturn]] void throw_type_error(const char* msg);
  [[noreturn]] void throw_internal_error(const char* msg);

  template <typename LazyT>
  void spawn_lazy(LazyT&& lazy)
  {
    std::move(lazy).via(&this->ex).start(
      [](async_simple::Try<void>&& t) {
        if (t.hasError()) {
          try {
            std::rethrow_exception(t.getException());
          } catch (const std::exception& e) {
            spdlog_lazy_error(e.what());
          } catch (...) {
            spdlog_lazy_error("unknown");
          }
        }
      });
  }

  async_simple::coro::Lazy<void> async_sleep(std::chrono::milliseconds ms);

  // Block until Lazy completes; drives ioc + JS jobs.
  template <typename T>
  T block_on(async_simple::coro::Lazy<T> lazy)
  {
    std::optional<T> out;
    std::exception_ptr ep;
    ++pending_ops;
    std::move(lazy).via(&this->ex).start(
      [this, &out, &ep](async_simple::Try<T>&& t) {
        if (t.hasError()) {
          ep = t.getException();
        } else {
          out = std::move(t.value());
        }
        --pending_ops;
      });
    run_loop();
    if (ep) {
      std::rethrow_exception(ep);
    }
    return std::move(*out);
  }

  void block_on(async_simple::coro::Lazy<void> lazy)
  {
    std::exception_ptr ep;
    ++pending_ops;
    std::move(lazy).via(&this->ex).start(
      [this, &ep](async_simple::Try<void>&& t) {
        if (t.hasError()) {
          ep = t.getException();
        }
        --pending_ops;
      });
    run_loop();
    if (ep) {
      std::rethrow_exception(ep);
    }
  }

  bool install_runtime();

  // Load a list of embedded JS polyfills. Used by Host::install_runtime and
  // by feature modules (crypto, fetch) that need to expose native functions
  // before their JS wrappers are evaluated.
  struct EmbeddedJs {
    const char* name;
    const unsigned char* bytes;
    std::size_t size;
  };
  bool install_bootstrap_js(std::span<const EmbeddedJs> scripts);

  // drain_jobs=false keeps Promise microtasks pending (needed for WPT batch
  // load).
  // flags defaults to JS_EVAL_TYPE_GLOBAL; use JS_EVAL_TYPE_MODULE for
  // sources containing import/export statements.
  bool eval_source(
    std::string_view code,
    const char* filename,
    bool drain = true,
    int flags = JS_EVAL_TYPE_GLOBAL
    );
  bool eval_file(const char* path);

  // Evaluate as an ES module (for scripts with import/export).
  bool eval_module(const char* path);

  // HTTP server start
  void start_http_server(uint16_t port, qjs::Value handler);

  // Called from JS when a response is ready
  void send_http_response_native(qjs::Value data, int32_t conn_id);

  friend JSValue native_http_response(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
  friend JSValue native_create_server(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
  friend JSValue native_close_server(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
  friend JSValue native_stream(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
  friend JSValue native_stream_write(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
  friend JSValue native_stream_end(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

  void send_js_response(uint64_t conn_id, qjs::Value response);

 private:
  static void spdlog_lazy_error(const char* msg);

  int32_t next_timer_id_ = 1;
  std::unordered_map<int32_t, std::shared_ptr<asio::steady_timer>> active_timers_;
};
