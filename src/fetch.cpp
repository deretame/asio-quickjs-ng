#include "fetch.hpp"

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <spdlog/spdlog.h>

#include <string>
#include <string_view>
#include <utility>

namespace {

using async_simple::Promise;
using async_simple::coro::Lazy;
using curl_http::FetchOptions;
using curl_http::FetchResult;
using curl_http::HeaderPair;
using curl_http::Transfer;

constexpr unsigned char kJsAbortBytes[] = {
#embed "js/abort.js"
};
constexpr unsigned char kJsTextEncodingPolyfillBytes[] = {
#embed "js/text-encoding-polyfill.js"
};
constexpr unsigned char kJsWhatwgUrlPolyfillBytes[] = {
#embed "js/whatwg-url-polyfill.js"
};
constexpr unsigned char kJsBodyPolyfillBytes[] = {
#embed "js/body_polyfill.js"
};
constexpr unsigned char kJsHeadersBytes[] = {
#embed "js/headers.js"
};
constexpr unsigned char kJsRequestBytes[] = {
#embed "js/request.js"
};
constexpr unsigned char kJsResponseBytes[] = {
#embed "js/response.js"
};
constexpr unsigned char kJsFetchBytes[] = {
#embed "js/fetch.js"
};

struct EmbeddedJs {
  const char* name;
  const unsigned char* bytes;
  std::size_t size;
};

constexpr EmbeddedJs kBootstrapJs[] = {
    {"js/abort.js", kJsAbortBytes, sizeof(kJsAbortBytes)},
    {"js/text-encoding-polyfill.js", kJsTextEncodingPolyfillBytes, sizeof(kJsTextEncodingPolyfillBytes)},
    {"js/whatwg-url-polyfill.js", kJsWhatwgUrlPolyfillBytes, sizeof(kJsWhatwgUrlPolyfillBytes)},
    {"js/body_polyfill.js", kJsBodyPolyfillBytes, sizeof(kJsBodyPolyfillBytes)},
    {"js/headers.js", kJsHeadersBytes, sizeof(kJsHeadersBytes)},
    {"js/request.js", kJsRequestBytes, sizeof(kJsRequestBytes)},
    {"js/response.js", kJsResponseBytes, sizeof(kJsResponseBytes)},
    {"js/fetch.js", kJsFetchBytes, sizeof(kJsFetchBytes)},
};

std::string js_prop_string(JSContext* ctx, JSValueConst obj, const char* name) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsUndefined(v) || JS_IsNull(v) || JS_IsException(v)) {
    JS_FreeValue(ctx, v);
    return {};
  }
  const char* s = JS_ToCString(ctx, v);
  JS_FreeValue(ctx, v);
  if (!s) {
    return {};
  }
  std::string out(s);
  JS_FreeCString(ctx, s);
  return out;
}

FetchOptions parse_options(JSContext* ctx, JSValueConst opts) {
  FetchOptions o;
  if (!JS_IsObject(opts)) {
    JS_ThrowTypeError(ctx, "__nativeFetch expects an options object");
    throw qjs::detail::ConvertError{};
  }
  o.url = js_prop_string(ctx, opts, "url");
  if (o.url.empty()) {
    JS_ThrowTypeError(ctx, "fetch url missing");
    throw qjs::detail::ConvertError{};
  }
  o.method = js_prop_string(ctx, opts, "method");
  if (o.method.empty()) {
    o.method = "GET";
  }
  o.body = js_prop_string(ctx, opts, "body");

  JSValue follow = JS_GetPropertyStr(ctx, opts, "followRedirects");
  if (JS_IsBool(follow)) {
    o.follow_redirects = JS_ToBool(ctx, follow) != 0;
  }
  JS_FreeValue(ctx, follow);
  JSValue failRedir = JS_GetPropertyStr(ctx, opts, "failOnRedirect");
  if (JS_IsBool(failRedir)) {
    o.fail_on_redirect = JS_ToBool(ctx, failRedir) != 0;
  }
  JS_FreeValue(ctx, failRedir);

  JSValue headers = JS_GetPropertyStr(ctx, opts, "headers");
  if (JS_IsArray(headers)) {
    JSValue len_v = JS_GetPropertyStr(ctx, headers, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    for (int32_t i = 0; i < len; ++i) {
      JSValue pair =
          JS_GetPropertyUint32(ctx, headers, static_cast<uint32_t>(i));
      if (JS_IsArray(pair)) {
        JSValue n = JS_GetPropertyUint32(ctx, pair, 0);
        JSValue v = JS_GetPropertyUint32(ctx, pair, 1);
        const char* ns = JS_ToCString(ctx, n);
        const char* vs = JS_ToCString(ctx, v);
        if (ns && vs) {
          o.headers.push_back(HeaderPair{ns, vs});
        }
        if (ns) {
          JS_FreeCString(ctx, ns);
        }
        if (vs) {
          JS_FreeCString(ctx, vs);
        }
        JS_FreeValue(ctx, n);
        JS_FreeValue(ctx, v);
      }
      JS_FreeValue(ctx, pair);
    }
  }
  JS_FreeValue(ctx, headers);
  return o;
}

qjs::Value make_raw_result(Host* host, const FetchResult& r) {
  JSContext* ctx = host->js_raw();
  qjs::Value o = host->ctx.object();
  o.set("ok", host->ctx.new_bool(r.ok));
  o.set("status", host->ctx.new_int32(static_cast<int32_t>(r.status)));
  o.set("statusText", host->ctx.new_string(r.status_text));
  o.set("url", host->ctx.new_string(r.url));
  o.set("body", host->ctx.new_string(r.body));
  o.set("redirected", host->ctx.new_bool(r.redirected));
  o.set("aborted", host->ctx.new_bool(r.aborted));
  if (!r.error.empty()) {
    o.set("error", host->ctx.new_string(r.error));
  }

  JSValue arr = JS_NewArray(ctx);
  uint32_t idx = 0;
  for (const auto& h : r.headers) {
    JSValue pair = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, h.name.c_str()));
    JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, h.value.c_str()));
    JS_SetPropertyUint32(ctx, arr, idx++, pair);
  }
  o.set("headers", qjs::Value::take(ctx, arr));
  return o;
}

Lazy<void> native_fetch_coro(Host* host, FetchOptions options, uint64_t id,
                             qjs::Value resolve, qjs::Value reject) {
  FetchResult result =
      co_await fetch_api::async_fetch(*host, std::move(options), id);

  host->fetch_transfers.erase(id);

  qjs::Value raw = make_raw_result(host, result);
  qjs::Value ret = resolve.call(raw);
  if (ret.is_exception()) {
    host->ctx.dump_exception();
  }

  host->drain_jobs();
  --host->pending_ops;
  co_return;
}

bool install_bootstrap_js(Host& host) {
  for (const EmbeddedJs& script : kBootstrapJs) {
    std::string_view src{reinterpret_cast<const char*>(script.bytes),
                         script.size};
    qjs::Value ret = host.ctx.eval(src, script.name, JS_EVAL_TYPE_GLOBAL);
    if (ret.is_exception()) {
      spdlog::error("bootstrap failed: {}", script.name);
      host.ctx.dump_exception();
      return false;
    }
  }
  host.drain_jobs();
  return true;
}

}  // namespace

namespace fetch_api {

Lazy<FetchResult> async_fetch(Host& host, FetchOptions options, uint64_t id) {
  Promise<FetchResult> p;
  auto fut = p.getFuture();

  auto* tr = new Transfer();
  tr->multi = &host.multi;
  tr->options = std::move(options);
  tr->id = id;
  tr->complete = [p = std::move(p)](FetchResult result) mutable {
    p.setValue(std::move(result));
  };

  if (id != 0) {
    host.fetch_transfers[id] = tr;
  }

  if (!tr->easy) {
    FetchResult r;
    r.ok = false;
    r.error = "curl_easy_init failed";
    r.url = tr->options.url;
    if (id != 0) {
      host.fetch_transfers.erase(id);
    }
    tr->finish(std::move(r));
    delete tr;
    co_return co_await std::move(fut);
  }

  if (!tr->start()) {
    FetchResult r;
    r.ok = false;
    r.error = "curl_multi_add_handle failed";
    r.url = tr->options.url;
    if (id != 0) {
      host.fetch_transfers.erase(id);
    }
    tr->finish(std::move(r));
    delete tr;
    co_return co_await std::move(fut);
  }

  host.notify_curl();
  auto result = co_await std::move(fut);
  // Transfer is deleted in on_multi_messages after finish — but abort path
  // also finish(). Ownership: finish does not delete; messages path deletes.
  // For abort we must delete after complete callback.
  // Keep ownership: delete after await if still in map cleared.
  co_return result;
}

qjs::Value native_fetch_fn(Host* host, qjs::Value opts) {
  if (!host->multi) {
    host->throw_internal_error("curl multi missing");
  }

  FetchOptions options = parse_options(host->js_raw(), opts.raw());
  const uint64_t id = host->next_fetch_id++;

  auto cap = qjs::PromiseCapability::create(host->ctx);
  if (!cap) {
    return qjs::Value::take(host->js_raw(), JS_EXCEPTION);
  }

  ++host->pending_ops;
  host->spawn_lazy(native_fetch_coro(host, std::move(options), id,
                                     std::move(cap->resolve),
                                     std::move(cap->reject)));

  qjs::Value out = host->ctx.object();
  out.set("id", host->ctx.new_int32(static_cast<int32_t>(id)));
  out.set("promise", std::move(cap->promise));
  return out;
}

void native_fetch_abort_fn(Host* host, int32_t id) {
  auto it = host->fetch_transfers.find(static_cast<uint64_t>(id));
  if (it == host->fetch_transfers.end()) {
    return;
  }
  Transfer* tr = it->second;
  host->fetch_transfers.erase(it);
  if (!tr) {
    return;
  }
  if (!tr->finished) {
    tr->abort();
  }
  delete tr;
}

bool install(Host& host) {
  host.global().fn<&native_fetch_fn>("__nativeFetch");
  host.global().fn<&native_fetch_abort_fn>("__nativeFetchAbort");
  return install_bootstrap_js(host);
}

}  // namespace fetch_api
