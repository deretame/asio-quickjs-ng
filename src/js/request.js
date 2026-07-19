(function (global) {
  if (typeof global.Headers !== 'function') {
    throw new Error('request.js: Headers missing');
  }
  if (typeof global.AbortSignal !== 'function') {
    throw new Error('request.js: AbortSignal missing');
  }

  var METHODS_WITHOUT_BODY = {
    GET: true,
    HEAD: true,
  };

  function normalizeMethod(method) {
    var m = String(method);
    var upper = m.toUpperCase();
    if (
      upper === 'DELETE' ||
      upper === 'GET' ||
      upper === 'HEAD' ||
      upper === 'OPTIONS' ||
      upper === 'POST' ||
      upper === 'PUT' ||
      upper === 'PATCH'
    ) {
      return upper;
    }
    if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(m)) {
      throw new TypeError('Invalid method');
    }
    if (upper === 'CONNECT' || upper === 'TRACE' || upper === 'TRACK') {
      throw new TypeError('Invalid method');
    }
    return m;
  }

  function createRequestSignal(init, inputRequest) {
    var signal = new global.AbortSignal();
    var hasInitSignal =
      init && Object.prototype.hasOwnProperty.call(init, 'signal');
    if (hasInitSignal) {
      if (init.signal != null) {
        signal._follow(init.signal);
      }
    } else if (inputRequest && inputRequest.signal) {
      signal._follow(inputRequest.signal);
    }
    return signal;
  }

  function extractBody(body) {
    if (body == null) {
      return { text: null, contentType: null, stream: null };
    }
    if (
      typeof global.ReadableStream === 'function' &&
      body instanceof global.ReadableStream
    ) {
      // Stream body: no default Content-Type (WPT request-init-contenttype).
      return { text: '', contentType: null, stream: body };
    }
    if (typeof body === 'string') {
      return { text: body, contentType: 'text/plain;charset=UTF-8', stream: null };
    }
    if (typeof global.Blob === 'function' && body instanceof global.Blob) {
      return {
        text: body._data != null ? body._data : '',
        contentType: body.type || null,
        stream: null,
      };
    }
    if (
      typeof global.URLSearchParams === 'function' &&
      body instanceof global.URLSearchParams
    ) {
      return {
        text: body.toString(),
        contentType: 'application/x-www-form-urlencoded;charset=UTF-8',
        stream: null,
      };
    }
    if (typeof global.FormData === 'function' && body instanceof global.FormData) {
      return {
        text: body._toMultipart(),
        contentType: 'multipart/form-data; boundary=' + body._boundary,
        stream: null,
      };
    }
    if (body instanceof ArrayBuffer) {
      var v = new Uint8Array(body);
      var s = '';
      for (var i = 0; i < v.length; ++i) {
        s += String.fromCharCode(v[i]);
      }
      return { text: s, contentType: null, stream: null };
    }
    if (ArrayBuffer.isView && ArrayBuffer.isView(body)) {
      var u8 = new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
      var s2 = '';
      for (var j = 0; j < u8.length; ++j) {
        s2 += String.fromCharCode(u8[j]);
      }
      return { text: s2, contentType: null, stream: null };
    }
    return {
      text: String(body),
      contentType: 'text/plain;charset=UTF-8',
      stream: null,
    };
  }

  function defineRO(obj, key, value) {
    Object.defineProperty(obj, key, {
      value: value,
      writable: false,
      enumerable: true,
      configurable: true,
    });
  }

  class Request {
    constructor(input, init) {
      init = init || {};
      var url;
      var method = 'GET';
      var headersInit;
      var body = null;
      var inputRequest = null;

      if (typeof Request !== 'undefined' && input instanceof Request) {
        inputRequest = input;
        if (inputRequest.bodyUsed && init.body === undefined) {
          throw new TypeError('Already read');
        }
        url = input.url;
        method = input.method;
        headersInit = input.headers;
        body = input._body;
      } else if (
        input != null &&
        typeof input === 'object' &&
        typeof input.href === 'string'
      ) {
        // URL instance or URL-like
        url = String(input.href);
      } else if (input != null) {
        // USVString coercion (WPT may pass the URL constructor function)
        url = String(input);
      } else {
        throw new TypeError('Request input must be a string or Request');
      }

      if (init.window !== undefined && init.window !== null) {
        throw new TypeError('Request constructor: window is not null');
      }

      if (init.method !== undefined) {
        method = init.method;
      }
      method = normalizeMethod(method);

      if (init.mode === 'navigate') {
        throw new TypeError('Invalid mode');
      }
      if (init.mode === 'no-cors') {
        var simple = { GET: 1, HEAD: 1, POST: 1 };
        if (!simple[method]) {
          throw new TypeError('Invalid method for no-cors');
        }
      }
      if (init.cache === 'only-if-cached') {
        var modeForCache = init.mode !== undefined ? init.mode : 'cors';
        if (modeForCache !== 'same-origin') {
          throw new TypeError('Invalid cache mode combination');
        }
      }
      if (
        init.referrer !== undefined &&
        init.referrer !== 'about:client' &&
        init.referrer !== ''
      ) {
        var ref = String(init.referrer);
        if (ref.indexOf('http://:') === 0 || ref.indexOf('https://:') === 0) {
          throw new TypeError('Invalid referrer');
        }
      }
      var enums = {
        referrerPolicy: {
          '': 1,
          'no-referrer': 1,
          'no-referrer-when-downgrade': 1,
          origin: 1,
          'origin-when-cross-origin': 1,
          'same-origin': 1,
          'strict-origin': 1,
          'strict-origin-when-cross-origin': 1,
          'unsafe-url': 1,
        },
        mode: { 'same-origin': 1, 'no-cors': 1, cors: 1, navigate: 1 },
        credentials: { omit: 1, 'same-origin': 1, include: 1 },
        cache: {
          default: 1,
          'no-store': 1,
          reload: 1,
          'no-cache': 1,
          'force-cache': 1,
          'only-if-cached': 1,
        },
        redirect: { follow: 1, error: 1, manual: 1 },
      };
      for (const prop of Object.keys(enums)) {
        if (init[prop] !== undefined && !enums[prop][init[prop]]) {
          throw new TypeError('Invalid ' + prop);
        }
      }

      if (typeof input === 'string') {
        var u = String(input);
        if (u.indexOf('http://:') === 0 || u.indexOf('https://:') === 0) {
          throw new TypeError('Invalid URL');
        }
        if (/^https?:\/\/[^/]*@/i.test(u)) {
          throw new TypeError(
            'Request cannot be constructed from a URL that includes credentials'
          );
        }
      }

      if (init.headers !== undefined) {
        headersInit = init.headers;
      }
      var headers = new global.Headers(headersInit);
      var mode =
        init.mode || (inputRequest ? inputRequest.mode : undefined) || 'cors';
      if (mode === 'no-cors') {
        headers._setGuard('request-no-cors');
      } else {
        headers._setGuard('request');
      }

      if (init.body !== undefined) {
        body = init.body;
      }

      // Validate before disturbing input (WPT: construction failure must not set bodyUsed).
      if (body != null && METHODS_WITHOUT_BODY[method]) {
        throw new TypeError(
          'Request with ' + method + ' method cannot have a body'
        );
      }

      // Input Request with a body is always disturbed when used as input.
      if (inputRequest && inputRequest.body !== null) {
        inputRequest._setBodyUsed(true);
      }

      var extracted =
        body == null || body === undefined
          ? { text: null, contentType: null, stream: null }
          : extractBody(body);
      this._body = extracted.text;
      if (extracted.contentType && !headers.has('content-type')) {
        headers.append('content-type', extracted.contentType);
      }

      // bodyUsed is read-only to script; internals use _setBodyUsed.
      defineRO(this, 'bodyUsed', false);

      defineRO(this, 'headers', headers);
      defineRO(this, 'url', url);
      defineRO(this, 'method', method);
      var bodyObj = null;
      if (extracted.stream) {
        bodyObj = extracted.stream;
      } else if (this._body != null) {
        bodyObj =
          typeof global.ReadableStream === 'function'
            ? new global.ReadableStream()
            : { locked: false };
      }
      defineRO(this, 'body', bodyObj);
      defineRO(
        this,
        'cache',
        init.cache || (inputRequest ? inputRequest.cache : 'default')
      );
      defineRO(
        this,
        'credentials',
        init.credentials ||
          (inputRequest ? inputRequest.credentials : 'same-origin')
      );
      defineRO(this, 'destination', '');
      defineRO(
        this,
        'integrity',
        init.integrity || (inputRequest ? inputRequest.integrity : '')
      );
      defineRO(
        this,
        'keepalive',
        init.keepalive !== undefined
          ? !!init.keepalive
          : !!(inputRequest && inputRequest.keepalive)
      );
      defineRO(
        this,
        'mode',
        init.mode || (inputRequest ? inputRequest.mode : 'cors')
      );
      defineRO(
        this,
        'redirect',
        init.redirect || (inputRequest ? inputRequest.redirect : 'follow')
      );
      defineRO(
        this,
        'referrer',
        init.referrer !== undefined
          ? String(init.referrer)
          : inputRequest
            ? inputRequest.referrer
            : 'about:client'
      );
      defineRO(
        this,
        'referrerPolicy',
        init.referrerPolicy ||
          (inputRequest ? inputRequest.referrerPolicy : '')
      );
      defineRO(this, 'isReloadNavigation', false);
      defineRO(this, 'isHistoryNavigation', false);
      defineRO(this, 'duplex', 'half');

      Object.defineProperty(this, 'signal', {
        value: createRequestSignal(init, inputRequest),
        writable: false,
        enumerable: true,
        configurable: true,
      });
    }

    clone() {
      if (this.bodyUsed) {
        throw new TypeError('Already read');
      }
      return new Request(this, {});
    }

    arrayBuffer() {
      return this._consume().then(function (s) {
        var utf8 = unescape(encodeURIComponent(s));
        var buf = new ArrayBuffer(utf8.length);
        var view = new Uint8Array(buf);
        for (var i = 0; i < utf8.length; ++i) {
          view[i] = utf8.charCodeAt(i) & 0xff;
        }
        return buf;
      });
    }

    blob() {
      var self = this;
      return this.text().then(function (s) {
        var type = self.headers.get('content-type') || '';
        return new global.Blob([s], { type: type });
      });
    }

    bytes() {
      return this.arrayBuffer().then(function (buf) {
        return new Uint8Array(buf);
      });
    }

    json() {
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      if (this._body === null) {
        return Promise.reject(new SyntaxError('Unexpected end of JSON input'));
      }
      return this.text().then(function (s) {
        return JSON.parse(s);
      });
    }

    text() {
      return this._consume();
    }

    _setBodyUsed(v) {
      Object.defineProperty(this, 'bodyUsed', {
        value: !!v,
        writable: false,
        enumerable: true,
        configurable: true,
      });
    }

    _consume() {
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      // Null body: do not set bodyUsed (WPT request-consume-empty).
      if (this._body === null) {
        return Promise.resolve('');
      }
      this._setBodyUsed(true);
      return Promise.resolve(this._body);
    }

    formData() {
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      if (this._body === null) {
        var ct = (this.headers.get('content-type') || '').toLowerCase();
        if (ct.indexOf('application/x-www-form-urlencoded') >= 0) {
          return Promise.resolve(new global.FormData());
        }
        return Promise.reject(new TypeError('Could not parse FormData'));
      }
      var ct2 = (this.headers.get('content-type') || '').toLowerCase();
      if (ct2.indexOf('application/x-www-form-urlencoded') >= 0) {
        this._setBodyUsed(true);
        var fd = new global.FormData();
        if (this._body) {
          this._body.split('&').forEach(function (part) {
            if (!part) return;
            var eq = part.indexOf('=');
            if (eq < 0) fd.append(decodeURIComponent(part), '');
            else
              fd.append(
                decodeURIComponent(part.slice(0, eq)),
                decodeURIComponent(part.slice(eq + 1))
              );
          });
        }
        return Promise.resolve(fd);
      }
      return Promise.reject(new TypeError('Could not parse FormData'));
    }
  }

  global.Request = Request;
})(globalThis);
