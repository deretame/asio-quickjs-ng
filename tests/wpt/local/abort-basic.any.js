// META: title=AbortController / fetch abort (offline + no wptserve)
// META: global=window,worker

"use strict";

test(() => {
  const c = new AbortController();
  assert_false(c.signal.aborted, "new signal is not aborted");
  assert_equals(c.signal.constructor, AbortSignal);
  c.abort();
  assert_true(c.signal.aborted, "aborted after abort()");
  assert_equals(c.signal.reason.name, "AbortError");
}, "AbortController basic abort");

test(() => {
  const reason = new Error("custom");
  reason.name = "custom";
  const c = new AbortController();
  c.abort(reason);
  assert_equals(c.signal.reason, reason);
}, "AbortController abort with reason");

test(() => {
  const s = AbortSignal.abort();
  assert_true(s.aborted);
  assert_equals(s.reason.name, "AbortError");
}, "AbortSignal.abort() static");

test(() => {
  const request = new Request("about:blank");
  assert_true(Boolean(request.signal), "Signal member is present & truthy");
  assert_equals(request.signal.constructor, AbortSignal);
}, "Request objects have a signal property");

test(() => {
  const controller = new AbortController();
  controller.abort();
  const request = new Request("about:blank", { signal: controller.signal });
  assert_not_equals(request.signal, controller.signal, "Request has a new signal");
  assert_true(request.signal.aborted, "Request signal already aborted");
}, "Request follows aborted init signal");

promise_test(async (t) => {
  const controller = new AbortController();
  controller.abort();
  const p = fetch("data:,hello", { signal: controller.signal });
  await promise_rejects_dom(t, "AbortError", p);
}, "Aborting before fetch rejects with AbortError");

promise_test(async (t) => {
  const error1 = new Error("error1");
  error1.name = "error1";
  const controller = new AbortController();
  controller.abort(error1);
  const p = fetch("data:,hello", { signal: controller.signal });
  await promise_rejects_exactly(t, error1, p);
}, "Aborting before fetch rejects with abort reason");

promise_test(async (t) => {
  try {
    new Request("", { method: "TRACE" });
  } catch (err) {
    const controller = new AbortController();
    controller.abort();
    await promise_rejects_js(t, TypeError, fetch("", {
      method: "TRACE",
      signal: controller.signal,
    }));
    return;
  }
  assert_unreached("TRACE should throw");
}, "TypeError from request constructor takes priority over abort");

promise_test(async () => {
  const controller = new AbortController();
  const request = new Request("data:,abc", {
    method: "POST",
    signal: controller.signal,
    body: "hi",
  });
  controller.abort();
  const text = await request.text();
  assert_equals(text, "hi");
}, "Calling text() on an aborted request still works");

promise_test(async () => {
  const res = await fetch("data:,response%27s%20body");
  assert_equals(res.status, 200);
  assert_equals(res.type, "basic");
  assert_equals(await res.text(), "response's body");
}, "data: URL fetch works with abort infrastructure present");
