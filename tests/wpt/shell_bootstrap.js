// Minimal shell globals so official resources/testharness.js uses ShellTestEnvironment.
(function (g) {
  g.self = g;
  // No document => ShellTestEnvironment (not Window).
  g.GLOBAL = {
    isWindow: function () {
      return false;
    },
    isWorker: function () {
      return true;
    },
    isShadowRealm: function () {
      return false;
    },
  };
  g.__WPT_DONE__ = false;
  g.__WPT_RESULTS__ = null;
})(globalThis);
