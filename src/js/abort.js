(function (global) {
  function AbortError(message) {
    var err = new Error(message || 'The operation was aborted.');
    err.name = 'AbortError';
    return err;
  }

  // Minimal DOMException for promise_rejects_dom(..., "AbortError", ...)
  class DOMException extends Error {
    constructor(message, name) {
      super(message || '');
      this.name = name || 'Error';
      this.code = 20; // ABORT_ERR
    }
  }

  class AbortSignal {
    constructor() {
      this.aborted = false;
      this.reason = undefined;
      this.onabort = null;
      this._listeners = [];
    }

    static abort(reason) {
      var s = new AbortSignal();
      s._signalAbort(reason);
      return s;
    }

    static timeout(ms) {
      var s = new AbortSignal();
      var n = Number(ms);
      if (!Number.isFinite(n) || n < 0) {
        throw new TypeError('Invalid timeout');
      }
      if (typeof global.setTimeout === 'function') {
        global.setTimeout(function () {
          s._signalAbort(new DOMException('The operation timed out.', 'TimeoutError'));
        }, n);
      }
      return s;
    }

    addEventListener(type, callback) {
      if (type !== 'abort' || typeof callback !== 'function') {
        return;
      }
      this._listeners.push(callback);
    }

    removeEventListener(type, callback) {
      if (type !== 'abort') {
        return;
      }
      this._listeners = this._listeners.filter(function (fn) {
        return fn !== callback;
      });
    }

    dispatchEvent(ev) {
      if (!ev || ev.type !== 'abort') {
        return false;
      }
      if (typeof this.onabort === 'function') {
        this.onabort(ev);
      }
      var list = this._listeners.slice();
      for (var i = 0; i < list.length; ++i) {
        list[i].call(this, ev);
      }
      return true;
    }

    throwIfAborted() {
      if (this.aborted) {
        throw this.reason;
      }
    }

    /** @internal */
    _signalAbort(reason) {
      if (this.aborted) {
        return;
      }
      this.aborted = true;
      if (reason !== undefined) {
        this.reason = reason;
      } else {
        this.reason = new DOMException(
          'The operation was aborted.',
          'AbortError'
        );
      }
      this.dispatchEvent({ type: 'abort', target: this });
    }

    /** Follow another signal (Request construction). */
    _follow(parent) {
      if (!parent || typeof parent !== 'object') {
        return;
      }
      if (parent.aborted) {
        this._signalAbort(parent.reason);
        return;
      }
      var self = this;
      parent.addEventListener('abort', function () {
        self._signalAbort(parent.reason);
      });
    }
  }

  class AbortController {
    constructor() {
      this.signal = new AbortSignal();
    }

    abort(reason) {
      this.signal._signalAbort(reason);
    }
  }

  global.DOMException = DOMException;
  global.AbortSignal = AbortSignal;
  global.AbortController = AbortController;
  global.AbortError = AbortError;
})(globalThis);
