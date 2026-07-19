#include "function_registry.hpp"

#include "host.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <stdexcept>

namespace {

qjs::Value make_error(JSContext *ctx, const char *msg) {
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
  return qjs::Value::take(ctx, err);
}

async_simple::coro::Lazy<void>
handle_async_call(qjs::Value resolve, qjs::Value reject,
                  async_simple::coro::Lazy<qjs::Value> lazy) {
  JSContext *ctx = resolve.ctx();
  Host *host = static_cast<Host *>(JS_GetContextOpaque(ctx));
  try {
    qjs::Value result = co_await std::move(lazy);
    if (result.is_exception()) {
      qjs::Value exc = qjs::Ctx{ctx}.exception();
      qjs::Value ret = reject.call(exc);
      if (ret.is_exception()) {
        host->ctx.dump_exception();
      }
    } else {
      qjs::Value ret = resolve.call(result);
      if (ret.is_exception()) {
        host->ctx.dump_exception();
      }
    }
  } catch (const qjs::detail::ConvertError &) {
    qjs::Value exc = qjs::Ctx{ctx}.exception();
    qjs::Value ret = reject.call(exc);
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
  } catch (const std::exception &e) {
    qjs::Value err = make_error(ctx, e.what());
    qjs::Value ret = reject.call(err);
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
  }
  --host->pending_ops;
  co_return;
}

} // namespace

void FunctionRegistry::register_function(const std::string &name,
                                         SyncFunction fn) {
  sync_functions[name] = std::move(fn);
}

void FunctionRegistry::register_async_function(const std::string &name,
                                               AsyncFunction fn) {
  async_functions[name] = std::move(fn);
}

bool FunctionRegistry::has_function(const std::string &name) const {
  return sync_functions.contains(name) || async_functions.contains(name);
}

JSValue native_call(JSContext *ctx, JSValueConst /*this_val*/, int argc,
                    JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "call(name, ...args)");
  }

  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_EXCEPTION;
  }
  std::string name_str(name);
  JS_FreeCString(ctx, name);

  Host *host = static_cast<Host *>(JS_GetContextOpaque(ctx));
  if (!host) {
    return JS_ThrowInternalError(ctx, "call: host is null");
  }

  auto sync_it = host->registry.sync_functions.find(name_str);
  if (sync_it != host->registry.sync_functions.end()) {
    std::vector<qjs::Value> owned_args;
    owned_args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      owned_args.push_back(qjs::Value::dup(ctx, argv[i]));
    }
    try {
      qjs::Value result = sync_it->second(qjs::Ctx{ctx}, owned_args);
      return result.release();
    } catch (const qjs::detail::ConvertError &) {
      return JS_EXCEPTION;
    } catch (const std::exception &e) {
      return JS_Throw(ctx, make_error(ctx, e.what()).release());
    }
  }

  auto async_it = host->registry.async_functions.find(name_str);
  if (async_it != host->registry.async_functions.end()) {
    std::vector<qjs::Value> owned_args;
    owned_args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      owned_args.push_back(qjs::Value::dup(ctx, argv[i]));
    }

    auto cap = qjs::PromiseCapability::create(ctx);
    if (!cap) {
      return JS_EXCEPTION;
    }

    auto lazy = async_it->second(qjs::Ctx{ctx}, owned_args);
    ++host->pending_ops;
    host->spawn_lazy(handle_async_call(std::move(cap->resolve),
                                       std::move(cap->reject),
                                       std::move(lazy)));
    return cap->promise.release();
  }

  return JS_ThrowTypeError(ctx, "call: function not registered: %s",
                           name_str.c_str());
}
