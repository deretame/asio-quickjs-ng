// http.js - HTTP server adapter for asio-quickjs-ng
// This file is loaded as a global script (not a module).
// Use globalThis.createServer to create an HTTP server.
(function (global) {
  'use strict';

  var _handler = null;

  function createServer(handler) {
    _handler = handler;
    global.__httpHandler = {
      get: function () { return _handler; }
    };
    return {
      listen: function (port) {
        port = port || 3000;
        if (typeof global.__nativeCreateServer === 'function') {
          global.__nativeCreateServer(port);
        } else {
          throw new Error('__nativeCreateServer not available');
        }
        return this;
      },
      close: function (callback) {
        if (typeof global.__nativeCloseServer === 'function') {
          global.__nativeCloseServer();
        }
        if (typeof callback === 'function') {
          callback();
        }
      }
    };
  }

  if (typeof module !== 'undefined') {
    module.exports = { createServer: createServer };
  }

  global.createServer = createServer;
})(typeof globalThis !== 'undefined' ? globalThis : this);
