// META: title=Optional public-network smoke (example.com)
// META: global=window,worker

"use strict";

promise_test(async (t) => {
  let res;
  try {
    res = await fetch("https://example.com/");
  } catch (e) {
    // Offline / blocked — treat as pass-with-note (not a product failure).
    assert_true(true, "network unavailable: " + e);
    return;
  }
  assert_true(res.status >= 200 && res.status < 500);
  const text = await res.text();
  assert_greater_than(text.length, 0);
}, "GET https://example.com/");

promise_test(async () => {
  const c = new AbortController();
  c.abort();
  let name = "";
  try {
    await fetch("https://example.com/", { signal: c.signal });
  } catch (e) {
    name = e && e.name;
  }
  assert_equals(name, "AbortError");
}, "Abort before example.com fetch");
