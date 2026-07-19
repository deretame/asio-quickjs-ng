/* WPT vendor report hook for asio-quickjs-ng */
(function (global) {
  global.__WPT_RESULTS__ = null;
  add_completion_callback(function (tests, harness_status) {
    var out = {
      harness: harness_status ? harness_status.status : 0,
      harness_message: harness_status && harness_status.message
        ? String(harness_status.message)
        : '',
      tests: [],
    };
    for (var i = 0; i < tests.length; ++i) {
      var t = tests[i];
      out.tests.push({
        name: String(t.name),
        status: t.status,
        message: t.message ? String(t.message) : '',
      });
    }
    global.__WPT_RESULTS__ = out;
    global.__WPT_DONE__ = true;
  });
})(globalThis);
