#pragma once

#include <spdlog/spdlog.h>

#include <concepts>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <limits>
#include <utility>

extern "C" {
#include "quickjs.h"
}

namespace qjs {

class Value;
class Ctx;

// ---------------------------------------------------------------------------
// Non-owning context view (for native callbacks)
// ---------------------------------------------------------------------------
class Ctx {
public:
explicit Ctx(JSContext* ctx) : ctx_(ctx) {}

JSContext* get() const { return ctx_; }

template <typename T>
T* opaque() const
{
  return static_cast<T*>(JS_GetContextOpaque(ctx_));
}

Value object() const;
Value new_bool(bool v) const;
Value new_int32(int32_t v) const;
Value new_string(std::string_view s) const;
Value undefined() const;
Value exception() const;

void dump_exception() const;

[[noreturn]] void throw_type_error(const char* msg) const;
[[noreturn]] void throw_internal_error(const char* msg) const;

template <auto Fn>
Value func(const char* name) const;

private:
JSContext* ctx_ = nullptr;
};

// ---------------------------------------------------------------------------
// Value (RAII)
// ---------------------------------------------------------------------------
class Value {
public:
Value() = default;
Value(JSContext* ctx, JSValue v) : ctx_(ctx), v_(v) {}

~Value() { reset(); }

Value(const Value& o)
  : ctx_(o.ctx_), v_(o.ctx_ ? JS_DupValue(o.ctx_, o.v_) : o.v_) {}

Value(Value&& o) noexcept : ctx_(o.ctx_), v_(o.v_)
{
  o.ctx_ = nullptr;
  o.v_ = JS_UNDEFINED;
}

Value& operator=(Value o) noexcept
{
  swap(o);
  return *this;
}

void swap(Value& o) noexcept
{
  std::swap(ctx_, o.ctx_);
  std::swap(v_, o.v_);
}

explicit operator bool() const {
  return ctx_ != nullptr;
}

JSContext* ctx() const { return ctx_; }

JSValue raw() const { return v_; }

JSValueConst cref() const { return v_; }

JSValue release()
{
  JSValue out = v_;
  ctx_ = nullptr;
  v_ = JS_UNDEFINED;
  return out;
}

void reset()
{
  if (ctx_) {
    JS_FreeValue(ctx_, v_);
  }
  ctx_ = nullptr;
  v_ = JS_UNDEFINED;
}

bool is_exception() const { return JS_IsException(v_); }

bool is_function() const { return ctx_ && JS_IsFunction(ctx_, v_); }

bool is_undefined() const { return JS_IsUndefined(v_); }

static Value undefined() { return {}; }

static Value dup(JSContext* ctx, JSValueConst v)
{
  return Value(ctx, JS_DupValue(ctx, v));
}

static Value take(JSContext* ctx, JSValue v) { return Value(ctx, v); }

// Low-level.
Value call_raw(JSValueConst this_val, int argc, JSValueConst* argv) const
{
  return take(ctx_, JS_Call(ctx_, v_, this_val, argc, argv));
}

// fn(), fn(a), fn(a, b, ...) — argc 由参数包推导；this = undefined
template <typename... Args>
Value call(const Args&... args) const
{
  return call_on_raw(JS_UNDEFINED, args...);
}

// method.call_on(obj, a, b)  — this = obj
template <typename... Args>
Value call_on(const Value& this_val, const Args&... args) const
{
  return call_on_raw(this_val.cref(), args...);
}

private:
template <typename... Args>
Value call_on_raw(JSValueConst this_val, const Args&... args) const
{
  static_assert(
    (std::same_as<Args, Value> && ...),
    "Value::call args must be qjs::Value");
  constexpr int n = static_cast<int>(sizeof...(Args));
  if constexpr (n == 0) {
    return call_raw(this_val, 0, nullptr);
  } else {
    JSValueConst argv[n] = {args.cref()...};
    return call_raw(this_val, n, argv);
  }
}

public:
std::optional<std::string> to_std_string() const
{
  if (!ctx_) {
    return std::nullopt;
  }
  const char* s = JS_ToCString(ctx_, v_);
  if (!s) {
    return std::nullopt;
  }
  std::string out(s);
  JS_FreeCString(ctx_, s);
  return out;
}

bool to_int32(int32_t& out) const
{
  return ctx_ && JS_ToInt32(ctx_, &out, v_) == 0;
}

void set(const char* name, Value value)
{
  JS_SetPropertyStr(ctx_, v_, name, value.release());
}

// g.fn<&native_fn>("name") — name once (creates + sets C function).
template <auto Fn>
void fn(const char* name);

// g.obj("console", [](Value &o) { o.fn<&print_fn>("log"); });
template <typename F>
void obj(const char* name, F&& setup)
{
  Value child = take(ctx_, JS_NewObject(ctx_));
  static_cast<F &&>(setup)(child);
  set(name, std::move(child));
}

Value get(const char* name) const
{
  return take(ctx_, JS_GetPropertyStr(ctx_, v_, name));
}

private:
JSContext* ctx_ = nullptr;
JSValue v_ = JS_UNDEFINED;
};

inline Value Ctx::object() const
{
  return Value::take(ctx_, JS_NewObject(ctx_));
}

inline Value Ctx::new_bool(bool v) const
{
  return Value::take(ctx_, JS_NewBool(ctx_, v));
}

inline Value Ctx::new_int32(int32_t v) const
{
  return Value::take(ctx_, JS_NewInt32(ctx_, v));
}

inline Value Ctx::new_string(std::string_view s) const
{
  return Value::take(ctx_, JS_NewStringLen(ctx_, s.data(), s.size()));
}

inline Value Ctx::undefined() const { return Value::take(ctx_, JS_UNDEFINED); }

inline Value Ctx::exception() const
{
  return Value::take(ctx_, JS_GetException(ctx_));
}

inline void Ctx::dump_exception() const
{
  Value exc = exception();
  auto s = exc.to_std_string();
  spdlog::error("JS exception: {}", s ? s->c_str() : "<unknown>");
}

inline void Ctx::throw_type_error(const char* msg) const
{
  JS_ThrowTypeError(ctx_, "%s", msg);
  throw std::runtime_error(msg);
}

inline void Ctx::throw_internal_error(const char* msg) const
{
  JS_ThrowInternalError(ctx_, "%s", msg);
  throw std::runtime_error(msg);
}

// ---------------------------------------------------------------------------
// Binding helpers (rquickjs-like)
// ---------------------------------------------------------------------------
struct This {
  Value value;
};

struct Args {
  JSContext* ctx = nullptr;
  int argc = 0;
  JSValueConst* argv = nullptr;

  int size() const { return argc; }

  Value operator[](int i) const { return Value::dup(ctx, argv[i]); }

};

namespace detail {

struct ConvertError {};

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

// ---- concepts: 只允许我们实现了转换的类型 ----

template <typename T>
struct is_js_value_type : std::false_type {};

template <>
struct is_js_value_type<Value> : std::true_type {};
template <>
struct is_js_value_type<std::string> : std::true_type {};
template <>
struct is_js_value_type<bool> : std::true_type {};
template <>
struct is_js_value_type<int32_t> : std::true_type {};
template <>
struct is_js_value_type<int64_t> : std::true_type {};
template <>
struct is_js_value_type<uint32_t> : std::true_type {};
template <>
struct is_js_value_type<float> : std::true_type {};
template <>
struct is_js_value_type<double> : std::true_type {};

template <typename T>
inline constexpr bool is_js_value_type_v =
  is_js_value_type<std::decay_t<T>>::value;

template <typename T>
concept JsValueType = is_js_value_type_v<T>;

// 从 JS_GetContextOpaque 注入的应用对象指针，例如 Host*
template <typename T>
concept OpaquePtr =
  std::is_pointer_v<T> && std::is_class_v<std::remove_pointer_t<T>> &&
  !std::same_as<std::remove_const_t<std::remove_pointer_t<T>>, Value> &&
  !std::same_as<std::remove_const_t<std::remove_pointer_t<T>>, Ctx> &&
  !std::same_as<std::remove_const_t<std::remove_pointer_t<T>>, This> &&
  !std::same_as<std::remove_const_t<std::remove_pointer_t<T>>, Args>;

template <typename T>
concept JsArgType =
  std::same_as<T, Ctx> || std::same_as<T, This> || std::same_as<T, Args> ||
  OpaquePtr<T> || JsValueType<T> ||
  (is_optional<T>::value && JsValueType<typename T::value_type>);

template <typename T>
concept JsReturnType =
  std::is_void_v<T> || std::same_as<T, JSValue> || is_js_value_type_v<T>;

template <typename T>
concept FreeFunctionPtr =
  std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

// ---- from_js / to_js ----

template <typename T>
requires std::same_as<T, Value>
inline Value from_js(JSContext* ctx, JSValueConst v)
{
  return Value::dup(ctx, v);
}

template <typename T>
requires std::same_as<T, std::string>
inline std::string from_js(JSContext* ctx, JSValueConst v)
{
  const char* s = JS_ToCString(ctx, v);
  if (!s) {
    throw ConvertError{};
  }
  std::string out(s);
  JS_FreeCString(ctx, s);
  return out;
}

template <typename T>
requires(std::integral<T> || std::floating_point<T>)
inline T from_js(JSContext* ctx, JSValueConst v)
{
  if constexpr (std::same_as<T, bool>) {
    return JS_ToBool(ctx, v);
  } else if constexpr (std::floating_point<T>) {
    double out = 0;
    if (JS_ToFloat64(ctx, &out, v)) {
      throw ConvertError{};
    }
    return static_cast<T>(out);
  } else if constexpr (std::same_as<T, int32_t>) {
    int32_t out = 0;
    if (JS_ToInt32(ctx, &out, v)) {
      throw ConvertError{};
    }
    return out;
  } else if constexpr (std::same_as<T, uint32_t>) {
    int64_t out = 0;
    if (JS_ToInt64(ctx, &out, v)) {
      throw ConvertError{};
    }
    if (out < 0 || out > std::numeric_limits<uint32_t>::max()) {
      throw ConvertError{};
    }
    return static_cast<uint32_t>(out);
  } else if constexpr (std::same_as<T, int64_t>) {
    int64_t out = 0;
    if (JS_ToInt64(ctx, &out, v)) {
      throw ConvertError{};
    }
    return out;
  }
}

template <JsValueType T>
std::optional<T> from_js_optional(JSContext* ctx, JSValueConst v)
{
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    return std::nullopt;
  }
  return from_js<T>(ctx, v);
}

template <JsArgType T>
T pull_arg(
  JSContext* ctx,
  JSValueConst this_val,
  int argc,
  JSValueConst* argv,
  int& index
)
{
  if constexpr (std::same_as<T, Ctx>) {
    return Ctx{ctx};
  } else if constexpr (std::same_as<T, This>) {
    return This{Value::dup(ctx, this_val)};
  } else if constexpr (std::same_as<T, Args>) {
    Args a{ctx, argc - index, argv + index};
    index = argc;
    return a;
  } else if constexpr (OpaquePtr<T>) {
    // 不消耗 JS 参数；从 context opaque 注入（注册前 JS_SetContextOpaque）
    auto* p = static_cast<T>(JS_GetContextOpaque(ctx));
    if (!p) {
      JS_ThrowInternalError(ctx, "context opaque is null");
      throw ConvertError{};
    }
    return p;
  } else if constexpr (is_optional<T>::value) {
    using U = typename T::value_type;
    if (index >= argc) {
      return std::nullopt;
    }
    return from_js_optional<U>(ctx, argv[index++]);
  } else {
    if (index >= argc) {
      JS_ThrowTypeError(ctx, "not enough arguments");
      throw ConvertError{};
    }
    return from_js<T>(ctx, argv[index++]);
  }
}

template <JsReturnType R>
JSValue to_js(JSContext* ctx, R&& r)
{
  using T = std::decay_t<R>;
  if constexpr (std::is_void_v<T>) {
    return JS_UNDEFINED;
  } else if constexpr (std::same_as<T, Value>) {
    return std::forward<R>(r).release();
  } else if constexpr (std::same_as<T, JSValue>) {
    return std::forward<R>(r);
  } else if constexpr (std::same_as<T, std::string> ||
    std::same_as<T, std::string_view> ||
    std::same_as<T, const char*>) {
    std::string_view s = r;
    return JS_NewStringLen(ctx, s.data(), s.size());
  } else if constexpr (std::same_as<T, bool>) {
    return JS_NewBool(ctx, r);
  } else if constexpr (std::floating_point<T>) {
    return JS_NewFloat64(ctx, static_cast<double>(r));
  } else if constexpr (std::same_as<T, int32_t>) {
    return JS_NewInt32(ctx, r);
  } else if constexpr (std::same_as<T, uint32_t>) {
    return JS_NewInt64(ctx, static_cast<int64_t>(r));
  } else if constexpr (std::same_as<T, int64_t>) {
    return JS_NewBigInt64(ctx, r);
  }
}

template <auto Fn>
struct fn_traits;

template <typename R, typename... Args, R(*Fn)(Args...)>
struct fn_traits<Fn> {
  using return_type = R;
  using args_tuple = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
  static constexpr bool ok = JsReturnType<R> && (JsArgType<Args> && ...);
};

// 兜底：非函数指针 / 无法解构时 ok = false
template <auto Fn>
struct fn_traits {
  using return_type = void;
  using args_tuple = std::tuple<>;
  static constexpr std::size_t arity = 0;
  static constexpr bool ok = false;
};

template <auto Fn>
concept NativeFn =
  requires {typename fn_traits<Fn>::args_tuple;} && fn_traits<Fn>::ok;

template <typename T>
constexpr bool is_injected_v =
  std::same_as<T, Ctx> || std::same_as<T, This> || OpaquePtr<T>;

template <typename Tuple, std::size_t I = 0>
constexpr int js_arity()
{
  if constexpr (I >= std::tuple_size_v<Tuple>) {
    return 0;
  } else {
    using T = std::tuple_element_t<I, Tuple>;
    if constexpr (std::same_as<T, Args>) {
      return 0;
    } else if constexpr (is_injected_v<T>) {
      return js_arity<Tuple, I + 1>();
    } else if constexpr (is_optional<T>::value) {
      return 0;
    } else {
      return 1 + js_arity<Tuple, I + 1>();
    }
  }
}

template <auto Fn, typename Tuple, std::size_t... I>
requires NativeFn<Fn>
JSValue invoke_impl(
  JSContext* ctx,
  JSValueConst this_val,
  int argc,
  JSValueConst* argv,
  std::index_sequence<I...>
)
{
  int index = 0;
  auto args = std::tuple<std::tuple_element_t<I, Tuple>...>{
    pull_arg<std::tuple_element_t<I, Tuple>>(
      ctx,
      this_val,
      argc,
      argv,
      index)...};

  using R = typename fn_traits<Fn>::return_type;
  if constexpr (std::is_void_v<R>) {
    std::apply(Fn, std::move(args));
    return JS_UNDEFINED;
  } else {
    return to_js(ctx, std::apply(Fn, std::move(args)));
  }
}

template <auto Fn>
requires NativeFn<Fn>
JSValue trampoline(
  JSContext* ctx,
  JSValueConst this_val,
  int argc,
  JSValueConst* argv
)
{
  using traits = fn_traits<Fn>;
  using Tuple = typename traits::args_tuple;
  try {
    return invoke_impl<Fn, Tuple>(
      ctx,
      this_val,
      argc,
      argv,
      std::make_index_sequence<std::tuple_size_v<Tuple>>{});
  } catch (const ConvertError&) {
    return JS_EXCEPTION;
  } catch (const std::runtime_error&) {
    return JS_EXCEPTION;
  }
}

template <auto Fn>
requires NativeFn<Fn>
constexpr int length_of()
{
  using Tuple = typename fn_traits<Fn>::args_tuple;
  return js_arity<Tuple>();
}

}  // namespace detail

// rquickjs-like: Func<&fn>::create(ctx, "name")
// 约束：Fn 必须是自由函数指针，参数/返回值在白名单内
template <auto Fn>
requires detail::NativeFn<Fn>
struct Func {
  static Value create(JSContext* ctx, const char* name)
  {
    return Value::take(
      ctx,
      JS_NewCFunction(
      ctx,
      &detail::trampoline<Fn>,
      name,
      detail::length_of<Fn>()));
  }

  static Value create(Ctx ctx, const char* name)
  {
    return create(ctx.get(), name);
  }

};

template <auto Fn>
Value Ctx::func(const char* name) const
{
  static_assert(
    detail::NativeFn<Fn>,
    "Func<&fn>: unsupported signature (see JsArgType / JsReturnType)");
  return Func<Fn>::create(ctx_, name);
}

template <auto Fn>
void Value::fn(const char* name)
{
  static_assert(
    detail::NativeFn<Fn>,
    "Value::fn<&fn>: unsupported signature (see JsArgType / JsReturnType)");
  set(name, Func<Fn>::create(ctx_, name));
}

// ---------------------------------------------------------------------------
// Owning Runtime / Context
// ---------------------------------------------------------------------------
class Runtime {
public:
Runtime() : rt_(JS_NewRuntime()) {}

~Runtime()
{
  if (rt_) {
    JS_FreeRuntime(rt_);
  }
}

Runtime(const Runtime&) = delete;
Runtime& operator=(const Runtime&) = delete;

Runtime(Runtime&& o) noexcept : rt_(o.rt_) { o.rt_ = nullptr; }

Runtime& operator=(Runtime&& o) noexcept
{
  if (this != &o) {
    if (rt_) {
      JS_FreeRuntime(rt_);
    }
    rt_ = o.rt_;
    o.rt_ = nullptr;
  }
  return *this;
}

explicit operator bool() const {
  return rt_ != nullptr;
}
JSRuntime* get() const { return rt_; }

bool job_pending() const { return JS_IsJobPending(rt_); }

bool drain_jobs()
{
  bool ok = true;
  JSContext* job_ctx = nullptr;
  while (JS_IsJobPending(rt_)) {
    if (JS_ExecutePendingJob(rt_, &job_ctx) < 0) {
      ok = false;
      if (job_ctx) {
        Ctx{job_ctx}.dump_exception();
      }
    }
  }
  return ok;
}

private:
JSRuntime* rt_ = nullptr;
};

class Context {
public:
explicit Context(Runtime& rt) : ctx_(JS_NewContext(rt.get())) {}

~Context()
{
  if (ctx_) {
    JS_FreeContext(ctx_);
  }
}

Context(const Context&) = delete;
Context& operator=(const Context&) = delete;

explicit operator bool() const {
  return ctx_ != nullptr;
}
JSContext* get() const { return ctx_; }

Ctx ref() const { return Ctx{ctx_}; }

void set_opaque(void* p) { JS_SetContextOpaque(ctx_, p); }

template <typename T>
T* opaque() const
{
  return static_cast<T*>(JS_GetContextOpaque(ctx_));
}

Value global() { return Value::take(ctx_, JS_GetGlobalObject(ctx_)); }

Value object() { return ref().object(); }

Value new_bool(bool v) { return ref().new_bool(v); }

Value new_int32(int32_t v) { return ref().new_int32(v); }

Value new_string(std::string_view s) { return ref().new_string(s); }

Value eval(std::string_view code, const char* filename, int flags)
{
  return Value::take(
    ctx_,
    JS_Eval(ctx_, code.data(), code.size(), filename, flags));
}

void dump_exception() { ref().dump_exception(); }

template <auto Fn>
Value func(const char* name)
{
  return Func<Fn>::create(ctx_, name);
}

private:
JSContext* ctx_ = nullptr;
};

struct PromiseCapability {
  Value promise;
  Value resolve;
  Value reject;

  static std::optional<PromiseCapability> create(JSContext* ctx)
  {
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) {
      return std::nullopt;
    }
    PromiseCapability out;
    out.promise = Value::take(ctx, p);
    out.resolve = Value::take(ctx, funcs[0]);
    out.reject = Value::take(ctx, funcs[1]);
    return out;
  }

  static std::optional<PromiseCapability> create(Ctx ctx)
  {
    return create(ctx.get());
  }

  static std::optional<PromiseCapability> create(Context& ctx)
  {
    return create(ctx.get());
  }

};

}  // namespace qjs
