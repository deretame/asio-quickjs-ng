#pragma once

#include <async_simple/Promise.h>
#include <async_simple/Try.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "asio_executor.hpp"
#include "function_registry.hpp"
#include "qjs.hpp"

namespace curl_http {
struct Transfer;
}

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

  Host();
  explicit Host(std::string id);
  ~Host();

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  explicit operator bool() const { return rt && ctx; }

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

  // Trampoline-style overloads: auto-convert JS args / return values.
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

  [[noreturn]] void throw_type_error(const char* msg);
  [[noreturn]] void throw_internal_error(const char* msg);

  template <typename LazyT>
  void spawn_lazy(LazyT&& lazy)
  {
    std::move(lazy).via(&ex).start(
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
    std::move(lazy).via(&ex).start(
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
    std::move(lazy).via(&ex).start(
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
  // drain_jobs=false keeps Promise microtasks pending (needed for WPT batch
  // load).
  bool eval_source(
    std::string_view code,
    const char* filename,
    bool drain = true
    );
  bool eval_file(const char* path);

private:
  static void spdlog_lazy_error(const char* msg);
};
