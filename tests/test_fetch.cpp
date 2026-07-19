// Non-WPT fetch smoke tests (local HTTP server + C++ API).
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "curl_http.hpp"
#include "fetch.hpp"
#include "host.hpp"
#include "http_test_server.hpp"
#include "qjs.hpp"

namespace {

bool setup_host(Host& host) {
  return host && host.install_runtime() && fetch_api::install(host);
}

bool js_truthy(Host& host, qjs::Value v) {
  return JS_ToBool(host.js_raw(), v.raw()) != 0;
}

std::string js_str(qjs::Value v) {
  auto s = v.to_std_string();
  return s.value_or("");
}

void run_js_async(Host& host, const std::string& body) {
  std::string code = R"JS(
    globalThis.__done = false;
    globalThis.__err = "";
    globalThis.__assert = function (cond, msg) {
      if (!cond) throw new Error(msg || "assert failed");
    };
    (async () => {
  )JS";
  code += body;
  code += R"JS(
    })().then(() => { globalThis.__done = true; })
      .catch((e) => {
        globalThis.__err = String(e && e.message ? e.message : e);
        globalThis.__done = true;
      });
  )JS";
  ASSERT_TRUE(host.eval_source(code, "fetch_smoke.js"));
  host.run_loop();
  ASSERT_TRUE(js_truthy(host, host.global().get("__done")));
  EXPECT_EQ(js_str(host.global().get("__err")), "");
}

std::string quote_js(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

class FetchLocal : public ::testing::Test {
 protected:
  void SetUp() override {
    server_.start();
    ASSERT_TRUE(setup_host(host_));
    origin_ = server_.origin();
    ASSERT_TRUE(host_.eval_source(
        "globalThis.__ORIGIN = " + quote_js(origin_) + ";", "origin.js"));
  }
  void TearDown() override {
    host_.shutdown();
    server_.stop();
  }

  HttpTestServer server_;
  Host host_;
  std::string origin_;
};

TEST_F(FetchLocal, GetText) {
  run_js_async(host_, R"JS(
    const res = await fetch(__ORIGIN + "/text");
    __assert(res.status === 200 && res.ok);
    __assert((await res.text()) === "Hello WPT");
  )JS");
}

TEST_F(FetchLocal, PostEcho) {
  run_js_async(host_, R"JS(
    const res = await fetch(__ORIGIN + "/echo", {
      method: "POST", body: "ping-body",
      headers: { "Content-Type": "text/plain" },
    });
    __assert((await res.text()) === "ping-body");
  )JS");
}

TEST_F(FetchLocal, Http404Resolves) {
  run_js_async(host_, R"JS(
    const res = await fetch(__ORIGIN + "/status/404");
    __assert(res.status === 404 && res.ok === false);
  )JS");
}

TEST_F(FetchLocal, FollowRedirect) {
  run_js_async(host_, R"JS(
    const res = await fetch(__ORIGIN + "/redirect");
    __assert(res.redirected === true);
    __assert((await res.text()) === "Hello WPT");
  )JS");
}

TEST_F(FetchLocal, NetworkErrorRejects) {
  run_js_async(host_, R"JS(
    let rejected = false;
    try { await fetch("http://127.0.0.1:1/"); }
    catch (e) { rejected = e instanceof TypeError; }
    __assert(rejected);
  )JS");
}

TEST_F(FetchLocal, AbortBeforeStart) {
  run_js_async(host_, R"JS(
    const c = new AbortController();
    c.abort();
    let name = "";
    try { await fetch(__ORIGIN + "/text", { signal: c.signal }); }
    catch (e) { name = e && e.name; }
    __assert(name === "AbortError");
  )JS");
}

TEST_F(FetchLocal, AbortDuringFetch) {
  run_js_async(host_, R"JS(
    const c = new AbortController();
    const p = fetch(__ORIGIN + "/text", { signal: c.signal });
    c.abort();
    let name = "";
    try { await p; } catch (e) { name = e && e.name; }
    __assert(name === "AbortError");
  )JS");
}

TEST_F(FetchLocal, DataAndAboutSchemes) {
  run_js_async(host_, R"JS(
    const r = await fetch("data:,hi%20there");
    __assert(r.status === 200 && r.type === "basic");
    __assert((await r.text()) === "hi there");
    let failed = false;
    try { await fetch("about:blank"); } catch (e) { failed = e instanceof TypeError; }
    __assert(failed);
  )JS");
}

TEST(FetchCpp, AsyncOptionsPost) {
  HttpTestServer server;
  server.start();
  Host host;
  ASSERT_TRUE(setup_host(host));
  curl_http::FetchOptions opt;
  opt.url = server.origin() + "/echo";
  opt.method = "POST";
  opt.body = "cpp-body";
  opt.headers.push_back({"Content-Type", "text/plain"});
  auto r = host.block_on(fetch_api::async_fetch(host, std::move(opt)));
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.body, "cpp-body");
  host.shutdown();
  server.stop();
}
