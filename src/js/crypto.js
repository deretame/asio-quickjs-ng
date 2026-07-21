(function (global) {
  var native = global.__nativeCrypto || {};

  function isBinary(v) {
    return (
      v instanceof Uint8Array ||
      v instanceof ArrayBuffer ||
      (ArrayBuffer.isView && ArrayBuffer.isView(v)) ||
      (Array.isArray(v) && v.every(function (x) { return typeof x === 'number'; }))
    );
  }

  function latin1ToBytes(s) {
    var buf = new Uint8Array(s.length);
    for (var i = 0; i < s.length; ++i) {
      buf[i] = s.charCodeAt(i) & 0xff;
    }
    return buf;
  }

  function stringToUtf8(s) {
    var utf8 = unescape(encodeURIComponent(s));
    var buf = new Uint8Array(utf8.length);
    for (var i = 0; i < utf8.length; ++i) {
      buf[i] = utf8.charCodeAt(i) & 0xff;
    }
    return buf;
  }

  function hexToBytes(s) {
    s = s.replace(/[^0-9a-fA-F]/g, '');
    if (s.length % 2 !== 0) throw new Error('Invalid hex string');
    var buf = new Uint8Array(s.length / 2);
    for (var i = 0; i < buf.length; ++i) {
      buf[i] = parseInt(s.substr(i * 2, 2), 16);
    }
    return buf;
  }

  function base64ToBytes(s) {
    var table =
      'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    var map = {};
    for (var i = 0; i < table.length; ++i) map[table[i]] = i;
    var out = [];
    var val = 0;
    var valb = -8;
    for (var j = 0; j < s.length; ++j) {
      var c = s.charAt(j);
      if (c === '=') break;
      var idx = map[c];
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

  function toBytes(input, encoding) {
    if (input == null) {
      return new Uint8Array(0);
    }
    if (input instanceof Uint8Array) return input;
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (ArrayBuffer.isView && ArrayBuffer.isView(input)) {
      return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    }
    if (Array.isArray(input)) {
      return new Uint8Array(input);
    }
    if (input instanceof global.Buffer) return input._u8;
    if (typeof input === 'string') {
      encoding = (encoding || 'utf8').toLowerCase();
      if (encoding === 'hex') return hexToBytes(input);
      if (encoding === 'base64') return base64ToBytes(input);
      if (encoding === 'latin1' || encoding === 'binary') return latin1ToBytes(input);
      return stringToUtf8(input);
    }
    throw new TypeError('Unsupported input type for crypto');
  }

  function toBuffer(u8) {
    return new global.Buffer(u8);
  }

  function toHex(u8) {
    var hex = '0123456789abcdef';
    var out = '';
    for (var i = 0; i < u8.length; ++i) {
      out += hex[u8[i] >> 4];
      out += hex[u8[i] & 0x0f];
    }
    return out;
  }

  function normalizeHash(name) {
    var s = String(name).toLowerCase().replace(/-/g, '');
    if (s === 'md5') return 'md5';
    if (s === 'sha1' || s === 'sha160') return 'sha1';
    if (s === 'sha256' || s === 'sha2256') return 'sha256';
    if (s === 'sha512' || s === 'sha2512') return 'sha512';
    throw new Error('Unsupported algorithm: ' + name);
  }

  function hashOnce(algorithm, data) {
    var bytes = toBytes(data);
    var alg = normalizeHash(algorithm);
    if (alg === 'md5') return native.md5(bytes);
    if (alg === 'sha1') return native.sha1(bytes);
    if (alg === 'sha256') return native.sha256(bytes);
    if (alg === 'sha512') return native.sha512(bytes);
    throw new Error('Unsupported algorithm: ' + algorithm);
  }

  function hmacOnce(algorithm, key, data) {
    var keyBytes = toBytes(key);
    var dataBytes = toBytes(data);
    var alg = normalizeHash(algorithm);
    if (alg === 'sha1') return native.hmacSha1(keyBytes, dataBytes);
    if (alg === 'sha256') return native.hmacSha256(keyBytes, dataBytes);
    if (alg === 'sha512') return native.hmacSha512(keyBytes, dataBytes);
    throw new Error('Unsupported algorithm: ' + algorithm);
  }

  function concatBytes(chunks) {
    var total = 0;
    for (var i = 0; i < chunks.length; ++i) total += chunks[i].length;
    var out = new Uint8Array(total);
    var off = 0;
    for (var j = 0; j < chunks.length; ++j) {
      out.set(chunks[j], off);
      off += chunks[j].length;
    }
    return out;
  }

  function createHash(algorithm) {
    var chunks = [];
    return {
      update: function (data, encoding) {
        chunks.push(toBytes(data, encoding));
        return this;
      },
      digest: function (encoding) {
        var all = concatBytes(chunks);
        var result = hashOnce(algorithm, all);
        if (encoding === 'buffer' || encoding === undefined) return toBuffer(result);
        return toBuffer(result).toString(encoding);
      },
    };
  }

  function createHmac(algorithm, key) {
    var chunks = [];
    return {
      update: function (data, encoding) {
        chunks.push(toBytes(data, encoding));
        return this;
      },
      digest: function (encoding) {
        var all = concatBytes(chunks);
        var result = hmacOnce(algorithm, key, all);
        if (encoding === 'buffer' || encoding === undefined) return toBuffer(result);
        return toBuffer(result).toString(encoding);
      },
    };
  }

  function digestHex(algorithm, input) {
    return new Promise(function (resolve) {
      var buf = hashOnce(algorithm, input);
      resolve(toBuffer(buf).toString('hex'));
    });
  }

  function hmacHex(algorithm, key, input) {
    return new Promise(function (resolve) {
      var buf = hmacOnce(algorithm, key, input);
      resolve(toBuffer(buf).toString('hex'));
    });
  }

  var crypto = {
    createHash: createHash,
    createHmac: createHmac,

    md5: function (input) { return digestHex('md5', input); },
    sha1: function (input) { return digestHex('sha1', input); },
    sha256: function (input) { return digestHex('sha256', input); },
    sha512: function (input) { return digestHex('sha512', input); },

    hmacSha1: function (key, input) { return hmacHex('sha1', key, input); },
    hmacSha256: function (key, input) { return hmacHex('sha256', key, input); },
    hmacSha512: function (key, input) { return hmacHex('sha512', key, input); },

    aesEcbPkcs7Encrypt: function (input, keyRaw) {
      return Promise.resolve(
        toBuffer(native.aesEcbEncrypt(toBytes(input), toBytes(keyRaw)))
      );
    },
    aesEcbPkcs7Decrypt: function (input, keyRaw) {
      return Promise.resolve(
        toBuffer(native.aesEcbDecrypt(toBytes(input), toBytes(keyRaw)))
      );
    },
    aesCbcPkcs7Encrypt: function (input, keyRaw, ivRaw) {
      return Promise.resolve(
        toBuffer(native.aesCbcEncrypt(toBytes(input), toBytes(keyRaw), toBytes(ivRaw)))
      );
    },
    aesCbcPkcs7Decrypt: function (input, keyRaw, ivRaw) {
      return Promise.resolve(
        toBuffer(native.aesCbcDecrypt(toBytes(input), toBytes(keyRaw), toBytes(ivRaw)))
      );
    },
    aesCbcPkcs7EncryptB64: function (payloadB64, keyRaw, ivRaw) {
      return crypto.aesCbcPkcs7Encrypt(base64ToBytes(payloadB64), keyRaw, ivRaw).then(function (b) {
        return b.toString('base64');
      });
    },
    aesCbcPkcs7DecryptB64: function (payloadB64, keyRaw, ivRaw) {
      return crypto.aesCbcPkcs7Decrypt(base64ToBytes(payloadB64), keyRaw, ivRaw).then(function (b) {
        return b.toString('base64');
      });
    },
    aesGcmEncrypt: function (input, keyRaw, nonceRaw, aad) {
      return Promise.resolve(
        toBuffer(native.aesGcmEncrypt(
          toBytes(input),
          toBytes(keyRaw),
          toBytes(nonceRaw),
          aad == null ? new Uint8Array(0) : toBytes(aad)
        ))
      );
    },
    aesGcmDecrypt: function (input, keyRaw, nonceRaw, aad) {
      return Promise.resolve(
        toBuffer(native.aesGcmDecrypt(
          toBytes(input),
          toBytes(keyRaw),
          toBytes(nonceRaw),
          aad == null ? new Uint8Array(0) : toBytes(aad)
        ))
      );
    },
    aesGcmEncryptB64: function (payloadB64, keyRaw, nonceRaw, aadB64) {
      var aad = aadB64 == null ? null : base64ToBytes(aadB64);
      return crypto.aesGcmEncrypt(base64ToBytes(payloadB64), keyRaw, nonceRaw, aad).then(function (b) {
        return b.toString('base64');
      });
    },
    aesGcmDecryptB64: function (payloadB64, keyRaw, nonceRaw, aadB64) {
      var aad = aadB64 == null ? null : base64ToBytes(aadB64);
      return crypto.aesGcmDecrypt(base64ToBytes(payloadB64), keyRaw, nonceRaw, aad).then(function (b) {
        return b.toString('base64');
      });
    },

    randomBytes: function (size) {
      return toBuffer(native.randomBytes(size));
    },
    randomUUID: function () {
      return native.randomUUID();
    },
    timingSafeEqual: function (a, b) {
      return native.timingSafeEqual(toBytes(a), toBytes(b));
    },
    pbkdf2Sync: function (password, salt, iterations, keyLen, digest) {
      digest = digest || 'sha256';
      return toBuffer(native.pbkdf2Sync(
        toBytes(password),
        toBytes(salt),
        iterations,
        keyLen,
        digest
      ));
    },
    pbkdf2: function (
      password,
      salt,
      iterations,
      keyLen,
      digest,
      callback
    ) {
      if (typeof digest === 'function') {
        callback = digest;
        digest = 'sha256';
      }
      digest = digest || 'sha256';
      setTimeout(function () {
        try {
          var result = toBuffer(native.pbkdf2Sync(
            toBytes(password),
            toBytes(salt),
            iterations,
            keyLen,
            digest
          ));
          if (callback) callback(null, result);
        } catch (e) {
          if (callback) callback(e);
        }
      }, 0);
    },
  };

  global.crypto = crypto;
})(globalThis);
