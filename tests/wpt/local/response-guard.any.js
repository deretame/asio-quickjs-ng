// META: title=Response headers immutable after fetch
// META: global=window,worker

"use strict";

const O = globalThis.__TEST_ORIGIN;

promise_test(async () => {
  const response = await fetch(O + "/resources/data.json");
  assert_throws_js(TypeError, () => {
    response.headers.append("name", "value");
  });
  assert_not_equals(
    response.headers.get("name"),
    "value",
    "response headers should be immutable"
  );
}, "Ensure response headers are immutable");

promise_test(async () => {
  const r = new Response("hi");
  const c = r.clone();
  assert_equals(await r.text(), "hi");
  assert_equals(await c.text(), "hi");
}, "Response.clone shares independent body copies");

promise_test(async () => {
  const r = new Response("data");
  await r.text();
  assert_true(r.bodyUsed);
  assert_throws_js(TypeError, () => r.clone());
}, "Cannot clone a disturbed response");
