(function (global) {
  var HEX = '0123456789abcdef';

  function isArrayBufferView(v) {
    return ArrayBuffer.isView && ArrayBuffer.isView(v);
  }

  function stringToUtf8(s) {
    var utf8 = unescape(encodeURIComponent(s));
    var buf = new Uint8Array(utf8.length);
    for (var i = 0; i < utf8.length; ++i) {
      buf[i] = utf8.charCodeAt(i) & 0xff;
    }
    return buf;
  }

  function utf8ToString(bytes) {
    var s = '';
    for (var i = 0; i < bytes.length; ++i) {
      s += String.fromCharCode(bytes[i]);
    }
    return decodeURIComponent(escape(s));
  }

  function latin1ToBytes(s) {
    var buf = new Uint8Array(s.length);
    for (var i = 0; i < s.length; ++i) {
      buf[i] = s.charCodeAt(i) & 0xff;
    }
    return buf;
  }

  function latin1ToString(bytes) {
    var s = '';
    for (var i = 0; i < bytes.length; ++i) {
      s += String.fromCharCode(bytes[i]);
    }
    return s;
  }

  function hexToBytes(s) {
    s = s.replace(/[^0-9a-fA-F]/g, '');
    if (s.length % 2 !== 0) {
      throw new Error('Invalid hex string');
    }
    var buf = new Uint8Array(s.length / 2);
    for (var i = 0; i < buf.length; ++i) {
      buf[i] = parseInt(s.substr(i * 2, 2), 16);
    }
    return buf;
  }

  function bytesToHex(bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; ++i) {
      var b = bytes[i];
      out += HEX[b >> 4];
      out += HEX[b & 0x0f];
    }
    return out;
  }

  var base64Table = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  function bytesToBase64(bytes) {
    var out = '';
    var i = 0;
    while (i + 3 <= bytes.length) {
      var b = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
      out += base64Table[(b >> 18) & 0x3f];
      out += base64Table[(b >> 12) & 0x3f];
      out += base64Table[(b >> 6) & 0x3f];
      out += base64Table[b & 0x3f];
      i += 3;
    }
    if (i < bytes.length) {
      var b = bytes[i] << 16;
      if (i + 1 < bytes.length) b |= bytes[i + 1] << 8;
      out += base64Table[(b >> 18) & 0x3f];
      out += base64Table[(b >> 12) & 0x3f];
      if (i + 1 < bytes.length) out += base64Table[(b >> 6) & 0x3f];
      else out += '=';
      out += '=';
    }
    return out;
  }

  function base64ToBytes(s) {
    var table = {};
    for (var i = 0; i < base64Table.length; ++i) table[base64Table[i]] = i;
    var out = [];
    var val = 0;
    var valb = -8;
    for (var j = 0; j < s.length; ++j) {
      var c = s.charAt(j);
      if (c === '=') break;
      var idx = table[c];
      if (idx === undefined) continue;
      val = (val << 6) + idx;
      valb += 6;
      if (valb >= 0) {
        out.push((val >> valb) & 0xff);
        valb -= 8;
      }
    }
    return new Uint8Array(out);
  }

  function decodeToBytes(input, encoding) {
    if (input == null) {
      return new Uint8Array(0);
    }
    if (input instanceof Uint8Array || input instanceof ArrayBuffer) {
      return input instanceof ArrayBuffer ? new Uint8Array(input) : input;
    }
    if (isArrayBufferView(input)) {
      return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    }
    if (Array.isArray(input)) {
      return new Uint8Array(input);
    }
    encoding = encoding || 'utf8';
    if (encoding === 'hex') return hexToBytes(input);
    if (encoding === 'base64') return base64ToBytes(input);
    if (encoding === 'latin1' || encoding === 'binary') return latin1ToBytes(input);
    return stringToUtf8(String(input));
  }

  function Buffer(value, encoding) {
    if (!(this instanceof Buffer)) {
      return new Buffer(value, encoding);
    }
    var bytes = decodeToBytes(value, encoding);
    this._u8 = bytes;
    this.length = bytes.length;
    for (var i = 0; i < bytes.length; ++i) {
      this[i] = bytes[i];
    }
  }

  Buffer.prototype.toString = function toString(encoding) {
    encoding = encoding || 'utf8';
    if (encoding === 'hex') return bytesToHex(this._u8);
    if (encoding === 'base64') return bytesToBase64(this._u8);
    if (encoding === 'latin1' || encoding === 'binary') return latin1ToString(this._u8);
    return utf8ToString(this._u8);
  };

  Buffer.from = function from(value, encoding) {
    return new Buffer(value, encoding);
  };

  Buffer.alloc = function alloc(size, fill, encoding) {
    var buf = new Uint8Array(size);
    if (fill !== undefined) {
      var bytes = decodeToBytes(fill, encoding);
      for (var i = 0; i < size; ++i) {
        buf[i] = bytes[i % bytes.length];
      }
    }
    return new Buffer(buf);
  };

  Buffer.isBuffer = function isBuffer(v) {
    return v instanceof Buffer;
  };

  Buffer.concat = function concat(list, totalLength) {
    if (!list || list.length === 0) return new Buffer(new Uint8Array(0));
    var len = 0;
    for (var i = 0; i < list.length; ++i) len += list[i].length;
    if (totalLength !== undefined) len = totalLength;
    var out = new Uint8Array(len);
    var off = 0;
    for (var j = 0; j < list.length; ++j) {
      var b = list[j]._u8;
      var n = Math.min(b.length, len - off);
      out.set(b.subarray(0, n), off);
      off += n;
      if (off >= len) break;
    }
    return new Buffer(out);
  };

  global.Buffer = Buffer;
})(globalThis);
