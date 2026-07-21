#include "fetch.hpp"

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <spdlog/spdlog.h>

#include <string>
#include <string_view>
#include <cstring>
#include <utility>
#include <vector>

#include "curl_runtime.hpp"
#include "js_embedded.hpp"

namespace {

using async_simple::Promise;
using async_simple::coro::Lazy;
using curl_http::FetchOptions;
using curl_http::FetchResult;
using curl_http::HeaderPair;
using curl_http::Transfer;
using namespace js_embedded;

std::vector<uint8_t> js_prop_bytes(JSContext* ctx, JSValueConst obj, const char* name)
{
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsUndefined(v) || JS_IsNull(v) || JS_IsException(v)) {
    JS_FreeValue(ctx, v);
    return {};
  }

  size_t size = 0;
  uint8_t* data = nullptr;
  if (JS_IsArrayBuffer(v)) {
    data = JS_GetArrayBuffer(ctx, &size, v);
  } else {
    data = JS_GetUint8Array(ctx, &size, v);
  }
  if (data) {
    std::vector<uint8_t> out(data, data + size);
    JS_FreeValue(ctx, v);
    return out;
  }

  const char* s = JS_ToCString(ctx, v);
  JS_FreeValue(ctx, v);
  if (!s) {
    return {};
  }
  std::vector<uint8_t> out(
    reinterpret_cast<const uint8_t*>(s),
    reinterpret_cast<const uint8_t*>(s) + std::strlen(s));
  JS_FreeCString(ctx, s);
  return out;
}

std::string js_prop_string(JSContext* ctx, JSValueConst obj, const char* name)
{
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

FetchOptions parse_options(JSContext* ctx, JSValueConst opts)
{
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
  o.body = js_prop_bytes(ctx, opts, "body");

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

qjs::Value make_raw_result(Host* host, FetchResult&& r)
{
  JSContext* ctx = host->js_raw();
  qjs::Value o = host->ctx.object();
  o.set("ok", host->ctx.new_bool(r.ok));
  o.set("status", host->ctx.new_int32(static_cast<int32_t>(r.status)));
  o.set("statusText", host->ctx.new_string(r.status_text));
  o.set("url", host->ctx.new_string(r.url));
  o.set("body", qjs::new_uint8_array(ctx, std::move(r.body)));
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

Lazy<void> native_fetch_coro(
  Host* host,
  FetchOptions options,
  uint64_t id,
  qjs::Value resolve,
  qjs::Value reject
)
{
  FetchResult result =
    co_await fetch_api::async_fetch(*host, std::move(options), id);

  host->fetch_transfers.erase(id);

  qjs::Value raw = make_raw_result(host, std::move(result));
  qjs::Value ret = resolve.call(raw);
  if (ret.is_exception()) {
    host->ctx.dump_exception();
  }

  host->drain_jobs();
  --host->pending_ops;
  co_return;
}

}  // namespace

namespace fetch_api {

Lazy<FetchResult> async_fetch(Host& host, FetchOptions options, uint64_t id)
{
  Promise<FetchResult> p;
  auto fut = p.getFuture();

  auto curl_client = std::make_shared<curl_http::Client>(host.ex);
  if (!curl_client || !*curl_client) {
    FetchResult r;
    r.ok = false;
    r.error = "curl client not available";
    r.url = options.url;
    co_return std::move(r);
  }

  auto* tr = new Transfer();
  tr->options = std::move(options);
  tr->id = id;
  tr->complete = [p = std::move(p)](FetchResult result) mutable {
      p.setValue(std::move(result));
    };

  if (id != 0) {
    host.fetch_transfers[id] = tr;
  }

  if (!curl_client->add_transfer(tr)) {
    FetchResult r;
    r.ok = false;
    r.error = "curl client not available";
    r.url = tr->options.url;
    if (id != 0) {
      host.fetch_transfers.erase(id);
    }
    tr->finish(std::move(r));
    delete tr;
    co_return std::move(r);
  }

  auto result = co_await std::move(fut);
  co_return result;
}

qjs::Value native_fetch_fn(Host* host, qjs::Value opts)
{
  FetchOptions options = parse_options(host->js_raw(), opts.raw());
  const uint64_t id = host->next_fetch_id++;

  auto cap = qjs::PromiseCapability::create(host->ctx);
  if (!cap) {
    return qjs::Value::take(host->js_raw(), JS_EXCEPTION);
  }

  ++host->pending_ops;
  host->spawn_lazy(
    native_fetch_coro(
    host,
    std::move(options),
    id,
    std::move(cap->resolve),
    std::move(cap->reject)));

  qjs::Value out = host->ctx.object();
  out.set("id", host->ctx.new_int32(static_cast<int32_t>(id)));
  out.set("promise", std::move(cap->promise));
  return out;
}

void native_fetch_abort_fn(Host* host, int32_t id)
{
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

bool install(Host& host)
{
  host.global().fn<&native_fetch_fn>("__nativeFetch");
  host.global().fn<&native_fetch_abort_fn>("__nativeFetchAbort");

  constexpr Host::EmbeddedJs k_fetch_bootstrap_js[] = {
    {"js/headers.js", kJsHeadersBytes, sizeof(kJsHeadersBytes)},
    {"js/request.js", kJsRequestBytes, sizeof(kJsRequestBytes)},
    {"js/response.js", kJsResponseBytes, sizeof(kJsResponseBytes)},
    {"js/fetch.js", kJsFetchBytes, sizeof(kJsFetchBytes)},
  };
  return host.install_bootstrap_js(k_fetch_bootstrap_js);
}

}  // namespace fetch_api
