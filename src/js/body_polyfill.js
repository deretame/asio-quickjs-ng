// Minimal Blob / URLSearchParams / FormData / ReadableStream for Fetch tests.
(function (global) {
  if (typeof global.Blob !== 'function') {
    class Blob {
      constructor(parts, options) {
        parts = parts || [];
        var data = '';
        for (var i = 0; i < parts.length; ++i) {
          var p = parts[i];
          if (p == null) {
            continue;
          }
          if (typeof p === 'string') {
            data += p;
          } else if (p instanceof ArrayBuffer) {
            data += String.fromCharCode.apply(null, new Uint8Array(p));
          } else if (ArrayBuffer.isView && ArrayBuffer.isView(p)) {
            data += String.fromCharCode.apply(
              null,
              new Uint8Array(p.buffer, p.byteOffset, p.byteLength)
            );
          } else if (typeof p === 'object' && typeof p._data === 'string') {
            data += p._data;
          } else {
            data += String(p);
          }
        }
        this._data = data;
        this.size = data.length;
        this.type = (options && options.type) || '';
      }
      text() {
        return Promise.resolve(this._data);
      }
      arrayBuffer() {
        var s = this._data;
        var buf = new ArrayBuffer(s.length);
        var v = new Uint8Array(buf);
        for (var i = 0; i < s.length; ++i) {
          v[i] = s.charCodeAt(i) & 0xff;
        }
        return Promise.resolve(buf);
      }
      slice(start, end, type) {
        start = start || 0;
        end = end === undefined ? this._data.length : end;
        return new Blob([this._data.slice(start, end)], {
          type: type || this.type,
        });
      }
    }
    global.Blob = Blob;
  }

  if (typeof global.URLSearchParams !== 'function') {
    class URLSearchParams {
      constructor(init) {
        this._pairs = [];
        if (init == null || init === '') {
          return;
        }
        if (typeof init === 'string') {
          var s = init.charAt(0) === '?' ? init.slice(1) : init;
          if (!s) {
            return;
          }
          s.split('&').forEach(function (part) {
            if (!part) return;
            var eq = part.indexOf('=');
            if (eq < 0) {
              this._pairs.push([decodeURIComponent(part.replace(/\+/g, ' ')), '']);
            } else {
              this._pairs.push([
                decodeURIComponent(part.slice(0, eq).replace(/\+/g, ' ')),
                decodeURIComponent(part.slice(eq + 1).replace(/\+/g, ' ')),
              ]);
            }
          }, this);
        } else if (typeof init === 'object') {
          Object.keys(init).forEach(function (k) {
            this._pairs.push([String(k), String(init[k])]);
          }, this);
        }
      }
      append(name, value) {
        this._pairs.push([String(name), String(value)]);
      }
      toString() {
        return this._pairs
          .map(function (p) {
            return encodeURIComponent(p[0]) + '=' + encodeURIComponent(p[1]);
          })
          .join('&');
      }
    }
    global.URLSearchParams = URLSearchParams;
  }

  if (typeof global.FormData !== 'function') {
    class FormData {
      constructor() {
        this._entries = [];
        this._boundary =
          '----asioqjs' + Math.random().toString(16).slice(2) + 'boundary';
      }
      append(name, value) {
        this._entries.push([String(name), String(value)]);
      }
      _toMultipart() {
        if (this._entries.length === 0) {
          return '';
        }
        var b = this._boundary;
        var parts = [];
        for (var i = 0; i < this._entries.length; ++i) {
          var n = this._entries[i][0];
          var v = this._entries[i][1];
          parts.push(
            '--' +
              b +
              '\r\nContent-Disposition: form-data; name="' +
              n +
              '"\r\n\r\n' +
              v +
              '\r\n'
          );
        }
        parts.push('--' + b + '--\r\n');
        return parts.join('');
      }
    }
    global.FormData = FormData;
  }

  // Minimal URL for Request(URL, init) WPT cases.
  if (typeof global.URL !== 'function') {
    class URL {
      constructor(url, base) {
        url = String(url);
        if (base != null && !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url)) {
          var b = String(base);
          if (b.charAt(b.length - 1) !== '/' && url.charAt(0) !== '/') {
            url = b.replace(/[^/]+$/, '') + url;
          } else if (url.charAt(0) === '/') {
            var m = b.match(/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\/[^/]+/);
            url = (m ? m[0] : b) + url;
          } else {
            url = b + url;
          }
        }
        this.href = url;
        this.toString = function () {
          return this.href;
        };
      }
    }
    global.URL = URL;
  }

  // Minimal ReadableStream so Request/Response.body passes instanceof checks.
  if (typeof global.ReadableStream !== 'function') {
    class ReadableStream {
      constructor() {
        this.locked = false;
        this._cancelled = false;
      }
      getReader() {
        this.locked = true;
        var self = this;
        return {
          read: function () {
            return Promise.resolve({ done: true, value: undefined });
          },
          cancel: function () {
            self._cancelled = true;
            self.locked = false;
            return Promise.resolve();
          },
          releaseLock: function () {
            self.locked = false;
          },
        };
      }
      cancel() {
        this._cancelled = true;
        return Promise.resolve();
      }
      tee() {
        return [new ReadableStream(), new ReadableStream()];
      }
    }
    global.ReadableStream = ReadableStream;
  }
})(globalThis);
