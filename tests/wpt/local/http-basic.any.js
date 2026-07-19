// META: title=HTTP basics on Node fixture (methods / status / redirect / concurrency)
// META: global=window,worker

"use strict";

const O = globalThis.__TEST_ORIGIN;

promise_test(async () => {
  const r = await fetch(O + "/text");
  assert_equals(r.status, 200);
  assert_true(r.ok);
  assert_equals(await r.text(), "Hello WPT");
}, "GET /text");

promise_test(async () => {
  const r = await fetch(O + "/method", { method: "HEAD" });
  assert_equals(r.status, 200);
  assert_equals(await r.text(), "");
}, "HEAD has empty body");

promise_test(async () => {
  const r = await fetch(O + "/method", { method: "PUT", body: "x" });
  assert_equals(await r.text(), "PUT");
}, "PUT method");

promise_test(async () => {
  const r = await fetch(O + "/method", { method: "DELETE" });
  assert_equals(await r.text(), "DELETE");
}, "DELETE method");

promise_test(async () => {
  const r = await fetch(O + "/method", { method: "PATCH", body: "p" });
  assert_equals(await r.text(), "PATCH");
}, "PATCH method");

promise_test(async () => {
  const r = await fetch(O + "/echo", {
    method: "POST",
    headers: { "Content-Type": "text/plain" },
    body: "hello-body",
  });
  assert_equals(await r.text(), "hello-body");
}, "POST body echo");

promise_test(async () => {
  const r = await fetch(O + "/status/404");
  assert_equals(r.status, 404);
  assert_false(r.ok);
  assert_equals(await r.text(), "status-body");
}, "404 resolves with ok=false");

promise_test(async () => {
  const r = await fetch(O + "/status/500");
  assert_equals(r.status, 500);
  assert_false(r.ok);
}, "500 resolves with ok=false");

promise_test(async () => {
  const r = await fetch(O + "/redirect/json");
  assert_true(r.redirected);
  assert_equals(r.status, 200);
  const j = await r.json();
  assert_equals(j.hello, "world");
}, "302 follow to JSON");

promise_test(async () => {
  // curl follows 302 and typically converts POST -> GET
  const r = await fetch(O + "/redirect/method", {
    method: "POST",
    body: "x",
    headers: { "Content-Type": "text/plain" },
  });
  assert_true(r.redirected);
  const m = await r.text();
  assert_equals(m, "GET", "POST+302 should land as GET on final URL");
}, "POST redirect becomes GET");

promise_test(async () => {
  const r = await fetch(O + "/headers/all", {
    headers: {
      "X-Custom": "abc",
      Accept: "text/plain",
      Host: "custom.example",
      Referer: "https://example.com/",
    },
  });
  const j = await r.json();
  assert_equals(j["x-custom"], "abc");
  assert_equals(j["accept"], "text/plain");
  // Non-browser runtime: custom Host / Referer are forwarded.
  assert_equals(j["host"], "custom.example");
  assert_equals(j["referer"], "https://example.com/");
}, "Custom request headers including browser-forbidden names are forwarded");

promise_test(async () => {
  const n = 20;
  const ps = [];
  for (let i = 0; i < n; ++i) {
    ps.push(
      fetch(O + "/concurrent?id=" + i).then(async (r) => {
        const j = await r.json();
        return j.id;
      })
    );
  }
  const ids = await Promise.all(ps);
  assert_equals(ids.length, n);
  const set = new Set(ids);
  assert_equals(set.size, n, "all concurrent ids distinct");
}, "Concurrent fetches");

promise_test(async () => {
  const r = await fetch(O + "/large?n=50000");
  const t = await r.text();
  assert_equals(t.length, 50000);
  assert_equals(t[0], "x");
}, "Large response body");

promise_test(async () => {
  let threw = false;
  try {
    await fetch(O + "/text", { redirect: "error", method: "GET" });
    // no redirect on /text — should succeed
  } catch (e) {
    threw = true;
  }
  assert_false(threw);

  let rejected = false;
  try {
    await fetch(O + "/redirect/json", { redirect: "error" });
  } catch (e) {
    rejected = e instanceof TypeError;
  }
  assert_true(rejected, "redirect:error should reject on redirect");
}, "redirect:error mode");
