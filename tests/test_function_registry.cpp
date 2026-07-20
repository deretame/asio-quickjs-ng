// Dynamic function registry tests: call(name, ...args) from JS.
#include <async_simple/coro/Lazy.h>
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>

#include "function_registry.hpp"
#include "host.hpp"
#include "qjs.hpp"

namespace {

bool setup_host(Host& host) {
  if (!host) {
    return false;
  }
  return host.install_runtime();
}

std::string js_str(qjs::Value v) {
  auto s = v.to_std_string();
  return s.value_or("");
}

}  // namespace

TEST(FunctionRegistry, SyncCall) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  // Trampoline-style: int32_t a, int32_t b -> int32_t.
  host.register_function("add",
                         [](int32_t a, int32_t b) -> int32_t { return a + b; });

  ASSERT_TRUE(host.eval_source(
      R"JS(
        globalThis.__syncResult = call("add", 2, 3);
      )JS",
      "sync_call.js"));

  int32_t value = 0;
  ASSERT_TRUE(host.global().get("__syncResult").to_int32(value));
  EXPECT_EQ(value, 5);
}

TEST(FunctionRegistry, SyncCallNotRegistered) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_FALSE(host.eval_source(
      R"JS(
        call("doesNotExist");
      )JS",
      "sync_not_found.js"));
}

TEST(FunctionRegistry, AsyncCall) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  // Trampoline-style async: Lazy<int32_t>(int32_t, int32_t).
  host.register_async_function(
      "asyncAdd",
      [&host](int32_t a, int32_t b) -> async_simple::coro::Lazy<int32_t> {
        co_await host.async_sleep(std::chrono::milliseconds(10));
        co_return a + b;
      });

  ASSERT_TRUE(host.eval_source(
      R"JS(
        globalThis.__asyncResult = null;
        globalThis.__asyncDone = false;
        globalThis.__asyncErr = "";
        (async () => {
          const r = await call("asyncAdd", 10, 20);
          globalThis.__asyncResult = r;
        })()
          .then(() => { globalThis.__asyncDone = true; })
          .catch((e) => {
            globalThis.__asyncErr = String(e && e.message ? e.message : e);
            globalThis.__asyncDone = true;
          });
      )JS",
      "async_call.js"));

  host.run_loop();

  EXPECT_TRUE(JS_ToBool(host.js_raw(), host.global().get("__asyncDone").raw()))
      << "async call did not complete";
  EXPECT_EQ(js_str(host.global().get("__asyncErr")), "");

  int32_t value = 0;
  ASSERT_TRUE(host.global().get("__asyncResult").to_int32(value));
  EXPECT_EQ(value, 30);
}

TEST(FunctionRegistry, SyncCallThrowsUserError) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  host.register_function(
      "boom", []() -> int32_t { throw std::runtime_error("boom from sync"); });

  ASSERT_TRUE(host.eval_source(
      R"JS(
        globalThis.__syncErr = null;
        try {
          call("boom");
        } catch (e) {
          globalThis.__syncErr = e;
        }
      )JS",
      "sync_user_error.js"));

  qjs::Value err = host.global().get("__syncErr");
  ASSERT_FALSE(err.is_undefined() || JS_IsNull(err.raw()));
  EXPECT_EQ(js_str(err.get("message")), "boom from sync");
}

TEST(FunctionRegistry, AsyncCallRejectsOnException) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  host.register_async_function("alwaysFail",
                               []() -> async_simple::coro::Lazy<int32_t> {
                                 throw std::runtime_error("boom from async");
                                 co_return 0;
                               });

  ASSERT_TRUE(host.eval_source(
      R"JS(
        globalThis.__asyncErr = null;
        (async () => {
          await call("alwaysFail");
        })().catch((e) => {
          globalThis.__asyncErr = e;
        });
      )JS",
      "async_reject.js"));

  host.run_loop();

  qjs::Value err = host.global().get("__asyncErr");
  ASSERT_FALSE(err.is_undefined());
  EXPECT_EQ(js_str(err.get("message")), "boom from async");
}

TEST(FunctionRegistry, GlobalSyncCall) {
  Host::register_global_function(
      "globalAdd",
      [](int32_t a, int32_t b) -> int32_t { return a + b; });

  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
      R"JS(
        globalThis.__globalSyncResult = call("globalAdd", 10, 20);
      )JS",
      "global_sync_call.js"));

  int32_t value = 0;
  ASSERT_TRUE(host.global().get("__globalSyncResult").to_int32(value));
  EXPECT_EQ(value, 30);
}

TEST(FunctionRegistry, GlobalSyncCallSharedByTwoHosts) {
  FunctionRegistry::register_global_function(
      "sharedMul",
      [](int32_t a, int32_t b) -> int32_t { return a * b; });

  Host host1;
  Host host2;
  ASSERT_TRUE(setup_host(host1));
  ASSERT_TRUE(setup_host(host2));

  ASSERT_TRUE(host1.eval_source(
      R"JS(globalThis.__r1 = call("sharedMul", 3, 4);)JS", "host1.js"));
  ASSERT_TRUE(host2.eval_source(
      R"JS(globalThis.__r2 = call("sharedMul", 5, 6);)JS", "host2.js"));

  int32_t r1 = 0;
  int32_t r2 = 0;
  ASSERT_TRUE(host1.global().get("__r1").to_int32(r1));
  ASSERT_TRUE(host2.global().get("__r2").to_int32(r2));
  EXPECT_EQ(r1, 12);
  EXPECT_EQ(r2, 30);
}

TEST(FunctionRegistry, GlobalAsyncCall) {
  Host::register_global_async_function(
      "globalAsyncAdd",
      [](int32_t a, int32_t b) -> async_simple::coro::Lazy<int32_t> {
        co_return a + b;
      });

  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
      R"JS(
        globalThis.__globalAsyncResult = null;
        globalThis.__globalAsyncDone = false;
        (async () => {
          globalThis.__globalAsyncResult = await call("globalAsyncAdd", 7, 8);
        })().then(() => { globalThis.__globalAsyncDone = true; });
      )JS",
      "global_async_call.js"));

  host.run_loop();

  EXPECT_TRUE(
      JS_ToBool(host.js_raw(), host.global().get("__globalAsyncDone").raw()))
      << "global async call did not complete";

  int32_t value = 0;
  ASSERT_TRUE(host.global().get("__globalAsyncResult").to_int32(value));
  EXPECT_EQ(value, 15);
}

TEST(FunctionRegistry, GlobalRegistrationAfterHostStarted) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  Host::register_global_function(
      "lateAdd",
      [](int32_t a, int32_t b) -> int32_t { return a + b; });

  ASSERT_TRUE(host.eval_source(
      R"JS(globalThis.__lateResult = call("lateAdd", 1, 2);)JS",
      "late_add.js"));

  int32_t value = 0;
  ASSERT_TRUE(host.global().get("__lateResult").to_int32(value));
  EXPECT_EQ(value, 3);
}

TEST(FunctionRegistry, GlobalBigIntReturn) {
  Host::register_global_function(
      "bigIntFn", []() -> int64_t { return 9007199254740993LL; });

  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
      R"JS(
        globalThis.__bigIntResult = call("bigIntFn");
        globalThis.__isBigInt = typeof globalThis.__bigIntResult === 'bigint';
      )JS",
      "bigint_return.js"));

  EXPECT_TRUE(
      JS_ToBool(host.js_raw(), host.global().get("__isBigInt").raw()));
}
