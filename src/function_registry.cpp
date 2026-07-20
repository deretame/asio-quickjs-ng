#include "function_registry.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <stdexcept>

#include "host.hpp"

namespace {

qjs::Value make_error(JSContext* ctx, const char* msg) {
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
  return qjs::Value::take(ctx, err);
}

async_simple::coro::Lazy<void> handle_async_call(
    qjs::Value resolve, qjs::Value reject,
    async_simple::coro::Lazy<qjs::Value> lazy) {
  JSContext* ctx = resolve.ctx();
  Host* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
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
  } catch (const qjs::detail::ConvertError&) {
    qjs::Value exc = qjs::Ctx{ctx}.exception();
    qjs::Value ret = reject.call(exc);
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
  } catch (const std::exception& e) {
    qjs::Value err = make_error(ctx, e.what());
    qjs::Value ret = reject.call(err);
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
  }
  --host->pending_ops;
  co_return;
}

}  // namespace

void FunctionRegistry::register_function(const std::string& name,
                                         SyncFunction fn) {
  sync_functions[name] = std::move(fn);
}

void FunctionRegistry::register_async_function(const std::string& name,
                                               AsyncFunction fn) {
  async_functions[name] = std::move(fn);
}

void FunctionRegistry::register_global_function(const std::string& name,
                                                 SyncFunction fn) {
  std::unique_lock<std::shared_mutex> lock(global_mutex);
  global_sync_functions[name] = std::move(fn);
}

void FunctionRegistry::register_global_async_function(const std::string& name,
                                                       AsyncFunction fn) {
  std::unique_lock<std::shared_mutex> lock(global_mutex);
  global_async_functions[name] = std::move(fn);
}

bool FunctionRegistry::has_function(const std::string& name) const {
  std::shared_lock<std::shared_mutex> lock(global_mutex);
  return sync_functions.contains(name) || async_functions.contains(name) ||
         global_sync_functions.contains(name) ||
         global_async_functions.contains(name);
}

std::optional<SyncFunction> FunctionRegistry::find_sync_function(
    const std::string& name) {
  std::shared_lock<std::shared_mutex> lock(global_mutex);
  auto it = sync_functions.find(name);
  if (it != sync_functions.end()) {
    return it->second;
  }
  auto git = global_sync_functions.find(name);
  if (git != global_sync_functions.end()) {
    return git->second;
  }
  return std::nullopt;
}

std::optional<AsyncFunction> FunctionRegistry::find_async_function(
    const std::string& name) {
  std::shared_lock<std::shared_mutex> lock(global_mutex);
  auto it = async_functions.find(name);
  if (it != async_functions.end()) {
    return it->second;
  }
  auto git = global_async_functions.find(name);
  if (git != global_async_functions.end()) {
    return git->second;
  }
  return std::nullopt;
}

JSValue native_call(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                    JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "call(name, ...args)");
  }

  const char* name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_EXCEPTION;
  }
  std::string name_str(name);
  JS_FreeCString(ctx, name);

  Host* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host) {
    return JS_ThrowInternalError(ctx, "call: host is null");
  }

  auto sync_fn = host->registry.find_sync_function(name_str);
  if (sync_fn) {
    std::vector<qjs::Value> owned_args;
    owned_args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      owned_args.push_back(qjs::Value::dup(ctx, argv[i]));
    }
    try {
      qjs::Value result = (*sync_fn)(qjs::Ctx{ctx}, host, owned_args);
      return result.release();
    } catch (const qjs::detail::ConvertError&) {
      return JS_EXCEPTION;
    } catch (const std::exception& e) {
      return JS_Throw(ctx, make_error(ctx, e.what()).release());
    }
  }

  auto async_fn = host->registry.find_async_function(name_str);
  if (async_fn) {
    std::vector<qjs::Value> owned_args;
    owned_args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      owned_args.push_back(qjs::Value::dup(ctx, argv[i]));
    }

    auto cap = qjs::PromiseCapability::create(ctx);
    if (!cap) {
      return JS_EXCEPTION;
    }

    auto lazy = (*async_fn)(qjs::Ctx{ctx}, host, owned_args);
    ++host->pending_ops;
    host->spawn_lazy(handle_async_call(
        std::move(cap->resolve), std::move(cap->reject), std::move(lazy)));
    return cap->promise.release();
  }

  return JS_ThrowTypeError(ctx, "call: function not registered: %s",
                           name_str.c_str());
}
