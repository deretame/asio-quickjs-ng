(function (global) {
  if (typeof global.Headers !== 'function') {
    throw new Error('response.js: Headers missing');
  }

  function isNullBodyStatus(status) {
    return status === 204 || status === 205 || status === 304;
  }

  function extractBody(body) {
    if (body == null) {
      return { text: null, contentType: null };
    }
    if (
      typeof global.ReadableStream === 'function' &&
      body instanceof global.ReadableStream
    ) {
      return { text: '', contentType: null, stream: body };
    }
    if (typeof body === 'string') {
      return { text: body, contentType: 'text/plain;charset=UTF-8' };
    }
    if (typeof global.Blob === 'function' && body instanceof global.Blob) {
      return {
        text: body._data != null ? body._data : '',
        contentType: body.type || null,
      };
    }
    if (
      typeof global.URLSearchParams === 'function' &&
      body instanceof global.URLSearchParams
    ) {
      return {
        text: body.toString(),
        contentType: 'application/x-www-form-urlencoded;charset=UTF-8',
      };
    }
    if (typeof global.FormData === 'function' && body instanceof global.FormData) {
      return {
        text: body._toMultipart(),
        contentType: 'multipart/form-data; boundary=' + body._boundary,
      };
    }
    if (body instanceof ArrayBuffer) {
      var v = new Uint8Array(body);
      var s = '';
      for (var i = 0; i < v.length; ++i) {
        s += String.fromCharCode(v[i]);
      }
      return { text: s, contentType: null };
    }
    if (ArrayBuffer.isView && ArrayBuffer.isView(body)) {
      var u8 = new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
      var s2 = '';
      for (var j = 0; j < u8.length; ++j) {
        s2 += String.fromCharCode(u8[j]);
      }
      return { text: s2, contentType: null };
    }
    return { text: String(body), contentType: 'text/plain;charset=UTF-8' };
  }

  class Response {
    constructor(body, init) {
      init = init || {};
      var headers = new global.Headers(init.headers);
      // Response headers use "response" guard (mutable, not request-forbidden list).
      headers._guard = 'response';

      var status = init.status !== undefined ? Number(init.status) : 200;
      if (!Number.isInteger(status) || status < 200 || status > 599) {
        throw new RangeError('Invalid status code');
      }
      this.status = status;

      this.statusText =
        init.statusText !== undefined ? String(init.statusText) : '';
      for (var i = 0; i < this.statusText.length; ++i) {
        var c = this.statusText.charCodeAt(i);
        if (c > 0xff || c === 0x0a || c === 0x0d) {
          throw new TypeError('Invalid statusText');
        }
      }

      this.ok = status >= 200 && status < 300;
      this.url = init.url == null ? '' : String(init.url);
      this.redirected = !!init.redirected;
      this.type = init.type || 'default';
      this.bodyUsed = false;

      var hasBodyArg = arguments.length > 0 && body !== undefined;
      if (!hasBodyArg || body === null) {
        this._body = null;
        this.body = null;
      } else if (isNullBodyStatus(status)) {
        throw new TypeError('Response with null body status cannot have body');
      } else {
        var extracted = extractBody(body);
        this._body = extracted.text;
        this.body = extracted.stream
          ? extracted.stream
          : typeof global.ReadableStream === 'function'
            ? new global.ReadableStream()
            : { locked: false };
        if (extracted.contentType && !headers.has('content-type')) {
          headers.append('content-type', extracted.contentType);
        }
      }

      Object.defineProperty(this, 'headers', {
        value: headers,
        writable: false,
        enumerable: true,
        configurable: true,
      });
    }

    clone() {
      if (this.bodyUsed) {
        throw new TypeError('Already read');
      }
      if (this._body === null) {
        var empty = new Response(null, {
          status: this.status,
          statusText: this.statusText,
          headers: this.headers,
          url: this.url,
          redirected: this.redirected,
          type: this.type,
        });
        empty._abortSignal = this._abortSignal;
        return empty;
      }
      var c = new Response(this._body, {
        status: this.status,
        statusText: this.statusText,
        headers: this.headers,
        url: this.url,
        redirected: this.redirected,
        type: this.type,
      });
      c._abortSignal = this._abortSignal;
      return c;
    }

    _checkAbort() {
      if (this._abortSignal && this._abortSignal.aborted) {
        return Promise.reject(this._abortSignal.reason);
      }
      return null;
    }

    // Null body: resolve empty and do NOT set bodyUsed (WPT response-consume-empty).
    _consumeString() {
      var aborted = this._checkAbort();
      if (aborted) {
        return aborted;
      }
      if (this._body === null) {
        return Promise.resolve('');
      }
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      this.bodyUsed = true;
      return Promise.resolve(this._body);
    }

    text() {
      return this._consumeString();
    }

    json() {
      var aborted = this._checkAbort();
      if (aborted) {
        return aborted;
      }
      if (this._body === null) {
        return Promise.reject(new SyntaxError('Unexpected end of JSON input'));
      }
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      this.bodyUsed = true;
      try {
        return Promise.resolve(JSON.parse(this._body));
      } catch (e) {
        return Promise.reject(e);
      }
    }

    arrayBuffer() {
      var aborted = this._checkAbort();
      if (aborted) {
        return aborted;
      }
      if (this._body === null) {
        return Promise.resolve(new ArrayBuffer(0));
      }
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      this.bodyUsed = true;
      var s = this._body;
      var utf8 = unescape(encodeURIComponent(s));
      var buf = new ArrayBuffer(utf8.length);
      var view = new Uint8Array(buf);
      for (var i = 0; i < utf8.length; ++i) {
        view[i] = utf8.charCodeAt(i) & 0xff;
      }
      return Promise.resolve(buf);
    }

    blob() {
      var aborted = this._checkAbort();
      if (aborted) {
        return aborted;
      }
      var type = this.headers.get('content-type') || '';
      if (this._body === null) {
        return Promise.resolve(new global.Blob([], { type: type }));
      }
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      this.bodyUsed = true;
      return Promise.resolve(new global.Blob([this._body], { type: type }));
    }

    bytes() {
      var aborted = this._checkAbort();
      if (aborted) {
        return aborted;
      }
      if (this._body === null) {
        return Promise.resolve(new Uint8Array(0));
      }
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      this.bodyUsed = true;
      var s = this._body;
      var utf8 = unescape(encodeURIComponent(s));
      var view = new Uint8Array(utf8.length);
      for (var i = 0; i < utf8.length; ++i) {
        view[i] = utf8.charCodeAt(i) & 0xff;
      }
      return Promise.resolve(view);
    }

    formData() {
      var aborted = this._checkAbort();
      if (aborted) {
        return aborted;
      }
      if (this._body === null) {
        var ct = (this.headers.get('content-type') || '').toLowerCase();
        if (ct.indexOf('application/x-www-form-urlencoded') >= 0) {
          return Promise.resolve(new global.FormData());
        }
        return Promise.reject(new TypeError('Could not parse FormData'));
      }
      if (this.bodyUsed) {
        return Promise.reject(new TypeError('Already read'));
      }
      var ct2 = (this.headers.get('content-type') || '').toLowerCase();
      if (ct2.indexOf('application/x-www-form-urlencoded') >= 0) {
        this.bodyUsed = true;
        var fd = new global.FormData();
        var usp = new global.URLSearchParams(this._body);
        // URLSearchParams polyfill may not iterate   parse manually
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

    static error() {
      var r = Object.create(Response.prototype);
      var h = new global.Headers();
      h._guard = 'immutable';
      Object.defineProperty(r, 'headers', {
        value: h,
        writable: false,
        enumerable: true,
        configurable: true,
      });
      r.status = 0;
      r.statusText = '';
      r.ok = false;
      r.url = '';
      r.redirected = false;
      r.type = 'error';
      r.bodyUsed = false;
      r._body = null;
      r.body = null;
      return r;
    }

    static redirect(url, status) {
      status = status === undefined ? 302 : status | 0;
      if ([301, 302, 303, 307, 308].indexOf(status) < 0) {
        throw new RangeError('Invalid redirect status');
      }
      url = String(url);
      if (url.indexOf('http://:') === 0 || url.indexOf('https://:') === 0) {
        throw new TypeError('Invalid URL');
      }
      if (url.indexOf('//') === 0) {
        throw new TypeError('Invalid URL');
      }
      return new Response(null, {
        status: status,
        headers: { Location: url },
      });
    }

    static json(data, init) {
      init = init || {};
      var str;
      try {
        str = JSON.stringify(data);
      } catch (e) {
        throw e;
      }
      if (str === undefined) {
        throw new TypeError('Response.json: data is not JSON-serializable');
      }
      var headers = new global.Headers(init.headers);
      if (!headers.has('content-type')) {
        headers.set('content-type', 'application/json');
      }
      return new Response(str, {
        status: init.status,
        statusText: init.statusText,
        headers: headers,
        url: init.url,
      });
    }
  }

  global.Response = Response;
})(globalThis);
