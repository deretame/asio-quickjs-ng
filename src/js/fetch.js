(function (global) {
  var nativeFetch = global.__nativeFetch;
  var nativeFetchAbort = global.__nativeFetchAbort;
  if (typeof nativeFetch !== 'function') {
    throw new Error('fetch.js: __nativeFetch missing');
  }
  if (typeof global.Request !== 'function') {
    throw new Error('fetch.js: Request missing');
  }
  if (typeof global.Response !== 'function') {
    throw new Error('fetch.js: Response missing');
  }
  if (typeof global.Headers !== 'function') {
    throw new Error('fetch.js: Headers missing');
  }

  function headersToPairs(headers) {
    var pairs = [];
    for (var i = 0; i < headers._list.length; ++i) {
      pairs.push([headers._list[i][0], headers._list[i][1]]);
    }
    return pairs;
  }

  function pairsToHeaders(pairs) {
    var h = new global.Headers();
    if (!pairs) {
      return h;
    }
    for (var i = 0; i < pairs.length; ++i) {
      var p = pairs[i];
      if (p && p.length >= 2) {
        try {
          h.append(p[0], p[1]);
        } catch (e) {}
      }
    }
    return h;
  }

  function decodeDataUrl(url) {
    // data:[<mediatype>][;base64],<data>
    if (url.indexOf('data:') !== 0) {
      return null;
    }
    var rest = url.slice(5);
    var comma = rest.indexOf(',');
    if (comma < 0) {
      throw new TypeError('Invalid data URL');
    }
    var meta = rest.slice(0, comma);
    var data = rest.slice(comma + 1);
    var isBase64 = /;base64$/i.test(meta) || /;base64;/i.test(meta);
    var mime = meta.replace(/;base64$/i, '').replace(/;base64;/i, ';');
    if (!mime) {
      mime = 'text/plain;charset=US-ASCII';
    }
    var body;
    if (isBase64) {
      body = global.atob ? global.atob(data) : base64Decode(data);
    } else {
      body = decodeURIComponent(data);
    }
    return { mime: mime, body: body };
  }

  function base64Decode(b64) {
    var chars =
      'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    var str = String(b64).replace(/=+$/, '');
    var out = '';
    for (var i = 0; i < str.length; i += 4) {
      var c1 = chars.indexOf(str.charAt(i));
      var c2 = chars.indexOf(str.charAt(i + 1));
      var c3 = chars.indexOf(str.charAt(i + 2));
      var c4 = chars.indexOf(str.charAt(i + 3));
      var n = (c1 << 18) | (c2 << 12) | ((c3 & 63) << 6) | (c4 & 63);
      out += String.fromCharCode((n >> 16) & 255);
      if (c3 !== -1 && str.charAt(i + 2) !== '') {
        out += String.fromCharCode((n >> 8) & 255);
      }
      if (c4 !== -1 && str.charAt(i + 3) !== '') {
        out += String.fromCharCode(n & 255);
      }
    }
    return out;
  }

  function responseFromRaw(request, raw) {
    var res = new global.Response(raw.body == null ? '' : raw.body, {
      status: raw.status | 0,
      statusText: raw.statusText == null ? '' : String(raw.statusText),
      headers: pairsToHeaders(raw.headers),
      url: raw.url == null ? request.url : String(raw.url),
      redirected: !!raw.redirected,
      type: 'basic',
    });
    // Network responses have immutable headers (WPT response-headers-guard).
    res.headers._guard = 'immutable';
    res._abortSignal = request.signal;
    return res;
  }

  function markBodyUsed(input, request) {
    if (input && typeof input === 'object' && input instanceof global.Request) {
      if (input._body != null && typeof input._setBodyUsed === 'function') {
        input._setBodyUsed(true);
      }
    }
    if (request && request._body != null && typeof request._setBodyUsed === 'function') {
      request._setBodyUsed(true);
    }
  }

  global.fetch = function fetch(input, init) {
    var request;
    try {
      request = new global.Request(input, init);
    } catch (e) {
      return Promise.reject(e);
    }

    if (request.signal && request.signal.aborted) {
      // Fetch still "uses" a non-null body even when aborting first (WPT).
      markBodyUsed(input, request);
      return Promise.reject(request.signal.reason);
    }

    var url = request.url;

    // about:   always network error (WPT scheme-about)
    if (url.indexOf('about:') === 0) {
      return Promise.reject(new TypeError('Failed to fetch'));
    }

    // data:   handled locally (WPT scheme-data)
    if (url.indexOf('data:') === 0) {
      try {
        if (request.method === 'HEAD') {
          var head = decodeDataUrl(url);
          return Promise.resolve(
            new global.Response('', {
              status: 200,
              statusText: 'OK',
              headers: { 'Content-Type': head.mime },
              url: url,
              type: 'basic',
            })
          );
        }
        var parsed = decodeDataUrl(url);
        return Promise.resolve(
          new global.Response(parsed.body, {
            status: 200,
            statusText: 'OK',
            headers: { 'Content-Type': parsed.mime },
            url: url,
            type: 'basic',
          })
        );
      } catch (e) {
        return Promise.reject(new TypeError('Failed to fetch'));
      }
    }

    var follow = request.redirect !== 'error' && request.redirect !== 'manual';
    var opts = {
      url: url,
      method: request.method,
      headers: headersToPairs(request.headers),
      body: request._body == null ? '' : request._body,
      followRedirects: follow,
      failOnRedirect: request.redirect === 'error',
    };

    var handle;
    try {
      handle = nativeFetch(opts);
    } catch (e) {
      return Promise.reject(e);
    }

    var id = handle.id;
    var promise = handle.promise;
    var abortCb = null;

    return new Promise(function (resolve, reject) {
      var settled = false;
      function settle(fn, arg) {
        if (settled) {
          return;
        }
        settled = true;
        request.signal.removeEventListener('abort', abortCb);
        fn(arg);
      }

      abortCb = function () {
        if (typeof nativeFetchAbort === 'function') {
          nativeFetchAbort(id | 0);
        }
        settle(reject, request.signal.reason);
      };

      if (request.signal.aborted) {
        abortCb();
        return;
      }
      request.signal.addEventListener('abort', abortCb);

      promise.then(
        function (raw) {
          if (request.signal.aborted || (raw && raw.aborted)) {
            settle(
              reject,
              request.signal.reason ||
                new global.DOMException(
                  'The operation was aborted.',
                  'AbortError'
                )
            );
            return;
          }
          if (!raw || raw.error) {
            settle(
              reject,
              new TypeError(
                (raw && raw.error) || 'Network request failed'
              )
            );
            return;
          }
          settle(resolve, responseFromRaw(request, raw));
        },
        function (err) {
          settle(reject, err);
        }
      );
    });
  };
})(globalThis);
