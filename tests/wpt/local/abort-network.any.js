// META: title=Abort + network (Node fixture server)
// META: global=window,worker
// Requires globalThis.__TEST_ORIGIN

"use strict";

const ORIGIN = globalThis.__TEST_ORIGIN;
const DATA = ORIGIN + "/resources/data.json";
const SLOW = ORIGIN + "/slow?ms=800";

const error1 = new Error("error1");
error1.name = "error1";

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort();
  await promise_rejects_dom(t, "AbortError", fetch(DATA, { signal: controller.signal }));
}, "Aborting rejects with AbortError");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort(error1);
  await promise_rejects_exactly(
    t,
    error1,
    fetch(DATA, { signal: controller.signal })
  );
}, "Aborting rejects with abort reason");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request(DATA, { signal: controller.signal });
  assert_not_equals(request.signal, controller.signal);
  assert_true(request.signal.aborted);
  await promise_rejects_dom(t, "AbortError", fetch(request));
}, "Signal on request object");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort(error1);
  const request = new Request(DATA, { signal: controller.signal });
  assert_equals(request.signal.reason, error1);
  await promise_rejects_exactly(t, error1, fetch(request));
}, "Signal on request object should also have abort reason");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request(DATA, { signal: controller.signal });
  const requestFromRequest = new Request(request);
  await promise_rejects_dom(t, "AbortError", fetch(requestFromRequest));
}, "Signal on request object created from request object");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request(DATA);
  const requestFromRequest = new Request(request, { signal: controller.signal });
  await promise_rejects_dom(t, "AbortError", fetch(requestFromRequest));
}, "Signal on request object created from request object, with signal on second request");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request(DATA, { signal: new AbortController().signal });
  const requestFromRequest = new Request(request, { signal: controller.signal });
  await promise_rejects_dom(t, "AbortError", fetch(requestFromRequest));
}, "Signal on second request overriding another");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request(DATA, { signal: controller.signal });
  await promise_rejects_dom(t, "AbortError", fetch(request, { method: "POST" }));
}, "Signal retained after unrelated properties are overridden by fetch");

promise_test(async () => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request(DATA, { signal: controller.signal });
  const data = await fetch(request, { signal: null }).then((r) => r.json());
  assert_equals(data.key, "value", "Fetch fully completes");
}, "Signal removed by setting to null");

promise_test(async () => {
  const controller = new AbortController();
  controller.abort();
  const log = [];
  await Promise.all([
    fetch(DATA, { signal: controller.signal }).then(
      () => assert_unreached("Fetch must not resolve"),
      () => log.push("fetch-reject")
    ),
    Promise.resolve().then(() => log.push("next-microtask")),
  ]);
  assert_array_equals(log, ["fetch-reject", "next-microtask"]);
}, "Already aborted signal rejects immediately");

promise_test(async () => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request(DATA, {
    signal: controller.signal,
    method: "POST",
    body: "foo",
    headers: { "Content-Type": "text/plain" },
  });
  await fetch(request).catch(() => {});
  assert_true(request.bodyUsed, "Body has been used");
}, "Request is still used if signal is aborted before fetching");

for (const bodyMethod of ["arrayBuffer", "blob", "bytes", "json", "text"]) {
  promise_test(async (t) => {
    const controller = new AbortController();
    const response = await fetch(DATA, { signal: controller.signal });
    controller.abort();
    const bodyPromise = response[bodyMethod]();
    const log = [];
    await Promise.all([
      bodyPromise.catch(() => log.push(`${bodyMethod}-reject`)),
      Promise.resolve().then(() => log.push("next-microtask")),
    ]);
    await promise_rejects_dom(t, "AbortError", bodyPromise);
    assert_array_equals(log, [`${bodyMethod}-reject`, "next-microtask"]);
  }, `response.${bodyMethod}() rejects if already aborted`);
}

promise_test(async (t) => {
  const controller = new AbortController();
  const res = await fetch(DATA, { signal: controller.signal });
  controller.abort();
  await promise_rejects_dom(t, "AbortError", res.text());
  await promise_rejects_dom(t, "AbortError", res.text());
}, "Call text() twice on aborted response");

promise_test(async (t) => {
  const controller = new AbortController();
  const p = fetch(SLOW, { signal: controller.signal });
  // abort on next macrotask so transfer is in-flight
  await Promise.resolve();
  controller.abort();
  await promise_rejects_dom(t, "AbortError", p);
}, "Abort during in-flight fetch");

promise_test(async () => {
  const res = await fetch(DATA);
  assert_equals(res.status, 200);
  assert_equals(res.type, "basic");
  const j = await res.json();
  assert_equals(j.key, "value");
}, "Successful JSON fetch against fixture server");

promise_test(async () => {
  const res = await fetch(ORIGIN + "/redirect/json");
  assert_true(res.redirected);
  assert_equals(await res.json().then((j) => j.hello), "world");
}, "Follow redirect on fixture server");

promise_test(async () => {
  const res = await fetch(ORIGIN + "/status/404");
  assert_equals(res.status, 404);
  assert_false(res.ok);
}, "HTTP 404 resolves (does not reject)");

promise_test(async () => {
  const res = await fetch(ORIGIN + "/echo", {
    method: "POST",
    headers: { "Content-Type": "text/plain" },
    body: "ping",
  });
  assert_equals(await res.text(), "ping");
}, "POST echo on fixture server");

promise_test(async () => {
  const res = await fetch(ORIGIN + "/headers", {
    headers: { "X-Test": "wpt", Accept: "application/json" },
  });
  const j = await res.json();
  assert_equals(j["x-test"], "wpt");
}, "Custom request headers reach server");
