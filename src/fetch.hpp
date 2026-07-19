#pragma once

#include "curl_http.hpp"
#include "host.hpp"
#include "qjs.hpp"

#include <async_simple/coro/Lazy.h>
#include <cstdint>

namespace fetch_api {

async_simple::coro::Lazy<curl_http::FetchResult>
async_fetch(Host &host, curl_http::FetchOptions options, uint64_t id = 0);

// Returns { id, promise } for abort support.
qjs::Value native_fetch_fn(Host *host, qjs::Value opts);
void native_fetch_abort_fn(Host *host, int32_t id);

bool install(Host &host);

} // namespace fetch_api
