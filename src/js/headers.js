(function (global) {
  function normalizeName(name) {
    name = String(name);
    if (name === '') {
      throw new TypeError('Invalid header name');
    }
    if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(name)) {
      throw new TypeError('Invalid header name');
    }
    return name.toLowerCase();
  }

  function normalizeValue(value) {
    value = String(value);
    for (var i = 0; i < value.length; ++i) {
      if (value.charCodeAt(i) > 0xff) {
        throw new TypeError('Invalid header value');
      }
    }
    value = value.replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
    if (/[\0\r\n]/.test(value)) {
      throw new TypeError('Invalid header value');
    }
    return value;
  }

  // Fetch "forbidden request-header" name
  var FORBIDDEN_REQUEST_HEADERS = {
    'accept-charset': 1,
    'accept-encoding': 1,
    'access-control-request-headers': 1,
    'access-control-request-method': 1,
    connection: 1,
    'content-length': 1,
    cookie: 1,
    cookie2: 1,
    date: 1,
    dnt: 1,
    expect: 1,
    host: 1,
    'keep-alive': 1,
    origin: 1,
    referer: 1,
    'set-cookie': 1,
    te: 1,
    trailer: 1,
    'transfer-encoding': 1,
    upgrade: 1,
    via: 1,
  };

  function isForbiddenRequestHeader(name) {
    var n = String(name).toLowerCase();
    if (FORBIDDEN_REQUEST_HEADERS[n]) {
      return true;
    }
    if (n.indexOf('proxy-') === 0 || n.indexOf('sec-') === 0) {
      return true;
    }
    return false;
  }

  function isNoCorsSafelistedRequestHeader(name, value) {
    var n = String(name).toLowerCase();
    var v = String(value);
    if (n === 'accept' || n === 'accept-language' || n === 'content-language') {
      return true;
    }
    if (n === 'content-type') {
      var mime = v.split(';')[0].trim().toLowerCase();
      return (
        mime === 'application/x-www-form-urlencoded' ||
        mime === 'multipart/form-data' ||
        mime === 'text/plain'
      );
    }
    return false;
  }

  class Headers {
    constructor(init) {
      this._list = [];
      this._guard = 'none'; // none | immutable | request | request-no-cors | response
      if (init === undefined) {
        return;
      }
      if (init === null || typeof init !== 'object') {
        throw new TypeError(
          "Failed to construct 'Headers': parameter is not an object"
        );
      }

      if (typeof init[Symbol.iterator] === 'function') {
        for (const pair of init) {
          if (pair == null || typeof pair[Symbol.iterator] !== 'function') {
            throw new TypeError('Invalid headers sequence');
          }
          const arr = Array.from(pair);
          if (arr.length !== 2) {
            throw new TypeError('Invalid headers sequence pair');
          }
          this.append(arr[0], arr[1]);
        }
        return;
      }

      for (const key of Object.keys(init)) {
        this.append(key, init[key]);
      }
    }

    _ensureMutable() {
      if (this._guard === 'immutable') {
        throw new TypeError('Headers are immutable');
      }
    }

    _forbiddenOrNoCors(name, value) {
      if (this._guard === 'request' && isForbiddenRequestHeader(name)) {
        return true;
      }
      if (this._guard === 'request-no-cors') {
        if (isForbiddenRequestHeader(name)) {
          return true;
        }
        if (!isNoCorsSafelistedRequestHeader(name, value)) {
          return true;
        }
      }
      return false;
    }

    /** Switch to request / request-no-cors and drop disallowed names. */
    _setGuard(guard) {
      this._guard = guard || 'none';
      if (this._guard === 'request' || this._guard === 'request-no-cors') {
        var next = [];
        for (var i = 0; i < this._list.length; ++i) {
          var n = this._list[i][0];
          var v = this._list[i][1];
          if (!this._forbiddenOrNoCors(n, v)) {
            next.push([n, v]);
          }
        }
        this._list = next;
      }
    }

    append(name, value) {
      this._ensureMutable();
      const n = normalizeName(name);
      const v = normalizeValue(value);
      if (this._forbiddenOrNoCors(n, v)) {
        return;
      }
      this._list.push([n, v]);
    }

    delete(name) {
      this._ensureMutable();
      const n = normalizeName(name);
      if (this._guard === 'request' && isForbiddenRequestHeader(n)) {
        return;
      }
      if (this._guard === 'request-no-cors' && isForbiddenRequestHeader(n)) {
        return;
      }
      // no-cors: can only delete safelisted names
      if (this._guard === 'request-no-cors') {
        // allow delete of existing; if not safelisted name, no-op
        if (
          n !== 'accept' &&
          n !== 'accept-language' &&
          n !== 'content-language' &&
          n !== 'content-type'
        ) {
          return;
        }
      }
      this._list = this._list.filter(function (p) {
        return p[0] !== n;
      });
    }

    get(name) {
      const n = normalizeName(name);
      const values = [];
      for (let i = 0; i < this._list.length; ++i) {
        if (this._list[i][0] === n) {
          values.push(this._list[i][1]);
        }
      }
      return values.length ? values.join(', ') : null;
    }

    has(name) {
      const n = normalizeName(name);
      for (let i = 0; i < this._list.length; ++i) {
        if (this._list[i][0] === n) {
          return true;
        }
      }
      return false;
    }

    set(name, value) {
      this._ensureMutable();
      const n = normalizeName(name);
      const v = normalizeValue(value);
      if (this._forbiddenOrNoCors(n, v)) {
        return;
      }
      let found = false;
      const next = [];
      for (let i = 0; i < this._list.length; ++i) {
        if (this._list[i][0] === n) {
          if (!found) {
            next.push([n, v]);
            found = true;
          }
        } else {
          next.push(this._list[i]);
        }
      }
      if (!found) {
        next.push([n, v]);
      }
      this._list = next;
    }

    forEach(callback, thisArg) {
      for (const [k, v] of this.entries()) {
        callback.call(thisArg, v, k, this);
      }
    }

    _combined() {
      const sorted = this._list.slice().sort(function (a, b) {
        if (a[0] < b[0]) return -1;
        if (a[0] > b[0]) return 1;
        return 0;
      });
      const seen = [];
      const map = new Map();
      for (let i = 0; i < sorted.length; ++i) {
        const k = sorted[i][0];
        const v = sorted[i][1];
        if (map.has(k)) {
          map.set(k, map.get(k) + ', ' + v);
        } else {
          map.set(k, v);
          seen.push(k);
        }
      }
      const items = [];
      for (let i = 0; i < seen.length; ++i) {
        items.push([seen[i], map.get(seen[i])]);
      }
      return items;
    }

    // Return real Iterator objects (not Generators) for WPT headers-basic.
    entries() {
      return makeHeadersIterator(this._combined(), 'entry');
    }

    keys() {
      return makeHeadersIterator(
        this._combined().map(function (p) {
          return p[0];
        }),
        'key'
      );
    }

    values() {
      return makeHeadersIterator(
        this._combined().map(function (p) {
          return p[1];
        }),
        'value'
      );
    }

    [Symbol.iterator]() {
      return this.entries();
    }

    getSetCookie() {
      return [];
    }

    _pairs() {
      return this._list.slice();
    }
  }

  // %IteratorPrototype% from array iterators (WPT checkIteratorProperties).
  var IteratorPrototype = Object.getPrototypeOf(
    Object.getPrototypeOf([][Symbol.iterator]())
  );

  function makeHeadersIterator(items, kind) {
    var i = 0;
    var iterator = Object.create(HeadersIteratorPrototype);
    iterator._items = items;
    iterator._index = 0;
    iterator._kind = kind;
    return iterator;
  }

  var HeadersIteratorPrototype = Object.create(IteratorPrototype, {
    next: {
      value: function next() {
        var items = this._items;
        var index = this._index;
        if (index >= items.length) {
          return { done: true, value: undefined };
        }
        this._index = index + 1;
        return { done: false, value: items[index] };
      },
      writable: true,
      enumerable: true,
      configurable: true,
    },
    [Symbol.iterator]: {
      value: function () {
        return this;
      },
      writable: true,
      enumerable: false,
      configurable: true,
    },
  });

  global.Headers = Headers;
  global.__isForbiddenRequestHeader = isForbiddenRequestHeader;
})(globalThis);
