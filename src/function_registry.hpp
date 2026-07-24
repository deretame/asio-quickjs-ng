#pragma once

#include <async_simple/coro/Lazy.h>

#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "qjs.hpp"

struct Host;

// Registered C++ function visible to JS as call(name, ...args).
// The Host* parameter lets the callback identify which instance invoked it.
// Returns a qjs::Value that is returned to JS synchronously.
using SyncFunction =
  std::function<qjs::Value(qjs::Ctx, Host*, const std::vector<qjs::Value>&)>;

// Registered C++ async function visible to JS as await call(name, ...args).
// Returns a Lazy<qjs::Value>; the result is resolved through a JS Promise.
using AsyncFunction = std::function<async_simple::coro::Lazy<qjs::Value>(
  qjs::Ctx, Host*, const std::vector<qjs::Value>&)>;

namespace function_registry_detail {

template <typename R, typename... Args>
struct callable_traits_base {
  using return_type = R;
  using args_tuple = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
};

// Pull signature from arbitrary callables (lambdas, functors, free functions).
template <typename Fn>
struct callable_traits : callable_traits<decltype(&Fn::operator())> {};

template <typename R, typename... Args>
struct callable_traits<R(Args...)> : callable_traits_base<R, Args...> {};

template <typename R, typename... Args>
struct callable_traits<R (*)(Args...)> : callable_traits_base<R, Args...> {};

template <typename C, typename R, typename... Args>
struct callable_traits<R (C::*)(Args...)> : callable_traits_base<R, Args...> {};

template <typename C, typename R, typename... Args>
struct callable_traits<R (C::*)(Args...) const>
  : callable_traits_base<R, Args...> {};

// MSVC: omit noexcept specializations to avoid partial-ordering ambiguity.

template <typename T>
struct is_lazy : std::false_type {};

template <typename T>
struct is_lazy<async_simple::coro::Lazy<T>> : std::true_type {};

template <typename T>
struct lazy_inner {
  using type = T;
};

template <typename T>
struct lazy_inner<async_simple::coro::Lazy<T>> {
  using type = T;
};

template <typename R>
qjs::Value to_value(JSContext* ctx, R&& r)
{
  using T = std::decay_t<R>;
  if constexpr (std::is_void_v<T>) {
    return qjs::Value::undefined();
  } else if constexpr (std::same_as<T, qjs::Value>) {
    return std::forward<R>(r);
  } else if constexpr (std::same_as<T, JSValue>) {
    return qjs::Value::take(ctx, std::forward<R>(r));
  } else if constexpr (std::same_as<T, std::string> ||
    std::same_as<T, std::string_view> ||
    std::same_as<T, const char*>) {
    std::string_view s = r;
    return qjs::Value::take(ctx, JS_NewStringLen(ctx, s.data(), s.size()));
  } else if constexpr (std::same_as<T, bool>) {
    return qjs::Value::take(ctx, JS_NewBool(ctx, r));
  } else if constexpr (std::floating_point<T>) {
    return qjs::Value::take(ctx, JS_NewFloat64(ctx, static_cast<double>(r)));
  } else if constexpr (std::same_as<T, int32_t>) {
    return qjs::Value::take(ctx, JS_NewInt32(ctx, r));
  } else if constexpr (std::same_as<T, uint32_t>) {
    return qjs::Value::take(ctx, JS_NewInt64(ctx, static_cast<int64_t>(r)));
  } else if constexpr (std::same_as<T, int64_t>) {
    return qjs::Value::take(ctx, JS_NewBigInt64(ctx, r));
  } else if constexpr (std::same_as<T, std::vector<uint8_t>>) {
    // Transfer ownership to JS as a Uint8Array (no extra copy).
    return qjs::new_uint8_array(ctx, std::move(r));
  } else {
    static_assert(std::false_type::value, "unsupported return type");
  }
}

template <typename T>
struct is_host_arg : std::false_type {};
template <>
struct is_host_arg<Host*> : std::true_type {};
template <>
struct is_host_arg<Host&> : std::true_type {};

template <typename Tuple>
constexpr std::size_t count_js_args()
{
  return []<std::size_t... I>(std::index_sequence<I...>) {
           return ((is_host_arg<std::tuple_element_t<I, Tuple>>::value ? 0 : 1) + ... + 0);
         }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename T>
T pull_arg_at(
  JSContext* ctx,
  Host* host,
  const std::vector<qjs::Value>& args,
  std::size_t& js_index
)
{
  if constexpr (is_host_arg<T>::value) {
    if constexpr (std::same_as<T, Host*>) {
      return host;
    } else {
      return *host;
    }
  } else {
    if (js_index >= args.size()) {
      JS_ThrowTypeError(ctx, "not enough arguments");
      throw qjs::detail::ConvertError{};
    }
    return qjs::detail::from_js<T>(ctx, args[js_index++].raw());
  }
}

template <typename Tuple, std::size_t... I>
auto build_call_args(
  JSContext* ctx,
  Host* host,
  const std::vector<qjs::Value>& args,
  std::index_sequence<I...>
)
{
  std::size_t js_index = 0;
  std::tuple<std::tuple_element_t<I, Tuple>...> result;
  ([&] {
      using T = std::tuple_element_t<I, Tuple>;
      std::get<I>(result) = pull_arg_at<T>(ctx, host, args, js_index);
    }(), ...);
  return result;
}

template <typename Tuple, typename R, typename Fn, std::size_t... I>
qjs::Value invoke_sync_impl(
  JSContext* ctx,
  Host* host,
  Fn& fn,
  const std::vector<qjs::Value>& args,
  std::index_sequence<I...> seq
)
{
  if (args.size() < count_js_args<Tuple>()) {
    JS_ThrowTypeError(ctx, "not enough arguments");
    throw qjs::detail::ConvertError{};
  }
  auto call_args = build_call_args<Tuple>(ctx, host, args, seq);
  if constexpr (std::is_void_v<R>) {
    std::apply(fn, std::move(call_args));
    return qjs::Value::undefined();
  } else {
    return to_value(ctx, std::apply(fn, std::move(call_args)));
  }
}

template <typename Tuple, typename R, typename Fn, std::size_t... I>
async_simple::coro::Lazy<qjs::Value> invoke_async_impl(
  JSContext* ctx,
  Host* host,
  Fn fn,
  std::vector<qjs::Value> args,
  std::index_sequence<I...> seq
)
{
  if (args.size() < count_js_args<Tuple>()) {
    JS_ThrowTypeError(ctx, "not enough arguments");
    throw qjs::detail::ConvertError{};
  }
  auto call_args = build_call_args<Tuple>(ctx, host, args, seq);
  auto lazy = std::apply(fn, std::move(call_args));
  R result = co_await std::move(lazy);
  co_return to_value(ctx, std::move(result));
}

template <typename Fn>
auto make_sync_wrapper(Fn&& fn)
{
  using Decayed = std::decay_t<Fn>;
  using Traits = callable_traits<Decayed>;
  using R = typename Traits::return_type;
  using Tuple = typename Traits::args_tuple;
  return [fn = std::forward<Fn>(fn)](
    qjs::Ctx ctx, Host* host,
    const std::vector<qjs::Value>& args) -> qjs::Value {
           return invoke_sync_impl<Tuple, R>(
             ctx.get(),
             host,
             fn,
             args,
             std::make_index_sequence<std::tuple_size_v<Tuple>>{});
         };
}

template <typename Fn>
auto make_async_wrapper(Fn&& fn)
{
  using Decayed = std::decay_t<Fn>;
  using Traits = callable_traits<Decayed>;
  using Return = typename Traits::return_type;
  using R = typename lazy_inner<Return>::type;
  using Tuple = typename Traits::args_tuple;
  static_assert(
    is_lazy<Return>::value,
    "register_async_function callback must return Lazy<T>");
  return [fn = std::forward<Fn>(fn)](
    qjs::Ctx ctx, Host* host,
    const std::vector<qjs::Value>& args)
         -> async_simple::coro::Lazy<qjs::Value> {
           return invoke_async_impl<Tuple, R>(
             ctx.get(),
             host,
             fn,
             args,
             std::make_index_sequence<std::tuple_size_v<Tuple>>{});
         };
}

}  // namespace function_registry_detail

struct FunctionRegistry {
  // Per-instance functions.
  std::unordered_map<std::string, SyncFunction> sync_functions;
  std::unordered_map<std::string, AsyncFunction> async_functions;

  // Global functions shared by every Host.  Registered before or while Hosts
  // are running; looked up as a fallback when an instance has no matching name.
  // Access is protected by global_mutex so multiple concurrent Host instances
  // can safely register and call global functions.
  static inline std::unordered_map<std::string, SyncFunction>
  global_sync_functions;
  static inline std::unordered_map<std::string, AsyncFunction>
  global_async_functions;
  static inline std::shared_mutex global_mutex;

  // Low-level: store a hand-rolled adapter in this instance.
  void register_function(const std::string& name, SyncFunction fn);
  void register_async_function(const std::string& name, AsyncFunction fn);

  // Low-level: store a hand-rolled adapter in the global registry.
  static void register_global_function(
    const std::string& name,
    SyncFunction fn
    );
  static void register_global_async_function(
    const std::string& name,
    AsyncFunction fn
    );

  // Trampoline-style: auto-convert JS args to C++ types and return values back
  // to JS. Sync callback signature: R(Args...). Async callback signature:
  // Lazy<R>(Args...).
  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, SyncFunction>)
  void register_function(const std::string& name, Fn&& fn)
  {
    sync_functions[name] =
      function_registry_detail::make_sync_wrapper(std::forward<Fn>(fn));
  }

  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, AsyncFunction>)
  void register_async_function(const std::string& name, Fn&& fn)
  {
    async_functions[name] =
      function_registry_detail::make_async_wrapper(std::forward<Fn>(fn));
  }

  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, SyncFunction>)
  static void register_global_function(const std::string& name, Fn&& fn)
  {
    std::unique_lock<std::shared_mutex> lock(global_mutex);
    global_sync_functions[name] =
      function_registry_detail::make_sync_wrapper(std::forward<Fn>(fn));
  }

  template <typename Fn>
  requires(!std::same_as<std::decay_t<Fn>, AsyncFunction>)
  static void register_global_async_function(const std::string& name, Fn&& fn)
  {
    std::unique_lock<std::shared_mutex> lock(global_mutex);
    global_async_functions[name] =
      function_registry_detail::make_async_wrapper(std::forward<Fn>(fn));
  }

  bool has_function(const std::string& name) const;

  // Lookup helpers: instance wins over global.  Return by value so the mutex
  // is released before the callback is invoked.
  std::optional<SyncFunction> find_sync_function(const std::string& name);
  std::optional<AsyncFunction> find_async_function(const std::string& name);
};

// JS native: call(name, ...args)
// Sync functions return their value directly. Async functions return a Promise.
JSValue native_call(Host* host, std::string name, qjs::Args args);
