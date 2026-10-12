// crypto (src/crypto/*.cc, on OpenSSL in node). There is no OpenSSL here.
// What is here: the hash functions programs use to name and check things
// (MD5, SHA-1, SHA-2), HMAC and PBKDF2 on them, random bytes from the kernel,
// the settings a TLS connection is made with (binding/tls.js makes it), and,
// for crypto.subtle and the global `crypto`, the browser's own Web
// Cryptography, which is the same interface node offers. Ciphers, key
// objects for public keys, signing, Diffie-Hellman and X.509 are not: their
// functions say so when called.
(function (hn) {
  "use strict";

  const { data, loop, web, SYS } = hn;
  const shape = data.bindings.crypto;

  // --- Hash functions. Each: state words, a 64- or 128-byte block function, the length at the end.
  const rotl = (x, n) => (x << n) | (x >>> (32 - n));
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));

  const md5S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
  const md5K = new Int32Array(64);
  for (let i = 0; i < 64; i++) {
    md5K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) | 0;
  }
  const md5Block = (h, m, at) => {
    const w = new Int32Array(16);
    for (let i = 0; i < 16; i++) {
      w[i] = m[at + 4 * i] | (m[at + 4 * i + 1] << 8) | (m[at + 4 * i + 2] << 16) | (m[at + 4 * i + 3] << 24);
    }
    let a = h[0], b = h[1], c = h[2], d = h[3];
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) & 15;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) & 15;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) & 15;
      }
      const t = d;
      d = c;
      c = b;
      b = (b + rotl((a + f + md5K[i] + w[g]) | 0, md5S[i])) | 0;
      a = t;
    }
    h[0] = (h[0] + a) | 0;
    h[1] = (h[1] + b) | 0;
    h[2] = (h[2] + c) | 0;
    h[3] = (h[3] + d) | 0;
  };

  const sha1Block = (h, m, at) => {
    const w = new Int32Array(80);
    for (let i = 0; i < 16; i++) {
      w[i] = (m[at + 4 * i] << 24) | (m[at + 4 * i + 1] << 16) | (m[at + 4 * i + 2] << 8) | m[at + 4 * i + 3];
    }
    for (let i = 16; i < 80; i++) {
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (let i = 0; i < 80; i++) {
      let f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5a827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdc;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6;
      }
      const t = (rotl(a, 5) + f + e + k + w[i]) | 0;
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = t;
    }
    h[0] = (h[0] + a) | 0;
    h[1] = (h[1] + b) | 0;
    h[2] = (h[2] + c) | 0;
    h[3] = (h[3] + d) | 0;
    h[4] = (h[4] + e) | 0;
  };

  // The constants of SHA-2 are the fractional parts of cube roots of primes: made, not listed.
  const primes = [];
  for (let n = 2; primes.length < 80; n++) {
    if (primes.every((p) => n % p)) {
      primes.push(n);
    }
  }
  const fraction64 = (p, root) => {
    // The first 64 bits after the point of the square or cube root of p, exactly, with whole numbers.
    const scaled = BigInt(p) << BigInt(64 * root);
    let lo = 0n, hi = 1n << 70n;
    while (lo < hi) {
      const mid = (lo + hi + 1n) >> 1n;
      if (mid ** BigInt(root) <= scaled) {
        lo = mid;
      } else {
        hi = mid - 1n;
      }
    }
    return lo & 0xffffffffffffffffn;
  };
  const k64 = primes.map((p) => fraction64(p, 3));
  const sha256K = new Int32Array(64);
  for (let i = 0; i < 64; i++) {
    sha256K[i] = Number(k64[i] >> 32n) | 0;
  }
  const sha512K = new Int32Array(160);
  for (let i = 0; i < 80; i++) {
    sha512K[2 * i] = Number(k64[i] >> 32n) | 0;
    sha512K[2 * i + 1] = Number(k64[i] & 0xffffffffn) | 0;
  }
  const initial64 = (count, skip) => primes.slice(skip, skip + count).map((p) => fraction64(p, 2));

  const sha256Block = (h, m, at) => {
    const w = new Int32Array(64);
    for (let i = 0; i < 16; i++) {
      w[i] = (m[at + 4 * i] << 24) | (m[at + 4 * i + 1] << 16) | (m[at + 4 * i + 2] << 8) | m[at + 4 * i + 3];
    }
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (let i = 0; i < 64; i++) {
      const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha256K[i] + w[i]) | 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      hh = g;
      g = f;
      f = e;
      e = (d + t1) | 0;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) | 0;
    }
    h[0] = (h[0] + a) | 0;
    h[1] = (h[1] + b) | 0;
    h[2] = (h[2] + c) | 0;
    h[3] = (h[3] + d) | 0;
    h[4] = (h[4] + e) | 0;
    h[5] = (h[5] + f) | 0;
    h[6] = (h[6] + g) | 0;
    h[7] = (h[7] + hh) | 0;
  };

  // SHA-512 works on 64-bit words: each is two 32-bit halves here, high then low.
  const sha512Block = (h, m, at) => {
    const w = new Int32Array(160);
    for (let i = 0; i < 32; i++) {
      w[i] = (m[at + 4 * i] << 24) | (m[at + 4 * i + 1] << 16) | (m[at + 4 * i + 2] << 8) | m[at + 4 * i + 3];
    }
    for (let i = 16; i < 80; i++) {
      const ah = w[2 * (i - 15)], al = w[2 * (i - 15) + 1];
      const s0h = ((ah >>> 1) | (al << 31)) ^ ((ah >>> 8) | (al << 24)) ^ (ah >>> 7);
      const s0l = ((al >>> 1) | (ah << 31)) ^ ((al >>> 8) | (ah << 24)) ^ ((al >>> 7) | (ah << 25));
      const bh = w[2 * (i - 2)], bl = w[2 * (i - 2) + 1];
      const s1h = ((bh >>> 19) | (bl << 13)) ^ ((bl >>> 29) | (bh << 3)) ^ (bh >>> 6);
      const s1l = ((bl >>> 19) | (bh << 13)) ^ ((bh >>> 29) | (bl << 3)) ^ ((bl >>> 6) | (bh << 26));
      let lo = (w[2 * (i - 16) + 1] >>> 0) + (s0l >>> 0) + (w[2 * (i - 7) + 1] >>> 0) + (s1l >>> 0);
      w[2 * i + 1] = lo | 0;
      w[2 * i] = (w[2 * (i - 16)] + s0h + w[2 * (i - 7)] + s1h + Math.floor(lo / 4294967296)) | 0;
    }
    const v = new Int32Array(h);
    for (let i = 0; i < 80; i++) {
      const eh = v[8], el = v[9];
      const S1h = ((eh >>> 14) | (el << 18)) ^ ((eh >>> 18) | (el << 14)) ^ ((el >>> 9) | (eh << 23));
      const S1l = ((el >>> 14) | (eh << 18)) ^ ((el >>> 18) | (eh << 14)) ^ ((eh >>> 9) | (el << 23));
      const chh = (eh & v[10]) ^ (~eh & v[12]);
      const chl = (el & v[11]) ^ (~el & v[13]);
      let lo = (v[15] >>> 0) + (S1l >>> 0) + (chl >>> 0) + (sha512K[2 * i + 1] >>> 0) + (w[2 * i + 1] >>> 0);
      const t1l = lo | 0;
      const t1h = (v[14] + S1h + chh + sha512K[2 * i] + w[2 * i] + Math.floor(lo / 4294967296)) | 0;
      const ah = v[0], al = v[1];
      const S0h = ((ah >>> 28) | (al << 4)) ^ ((al >>> 2) | (ah << 30)) ^ ((al >>> 7) | (ah << 25));
      const S0l = ((al >>> 28) | (ah << 4)) ^ ((ah >>> 2) | (al << 30)) ^ ((ah >>> 7) | (al << 25));
      const majh = (ah & v[2]) ^ (ah & v[4]) ^ (v[2] & v[4]);
      const majl = (al & v[3]) ^ (al & v[5]) ^ (v[3] & v[5]);
      lo = (S0l >>> 0) + (majl >>> 0);
      const t2l = lo | 0;
      const t2h = (S0h + majh + Math.floor(lo / 4294967296)) | 0;
      v[14] = v[12];
      v[15] = v[13];
      v[12] = v[10];
      v[13] = v[11];
      v[10] = eh;
      v[11] = el;
      lo = (v[7] >>> 0) + (t1l >>> 0);
      v[9] = lo | 0;
      v[8] = (v[6] + t1h + Math.floor(lo / 4294967296)) | 0;
      v[6] = v[4];
      v[7] = v[5];
      v[4] = v[2];
      v[5] = v[3];
      v[2] = ah;
      v[3] = al;
      lo = (t1l >>> 0) + (t2l >>> 0);
      v[1] = lo | 0;
      v[0] = (t1h + t2h + Math.floor(lo / 4294967296)) | 0;
    }
    for (let i = 0; i < 8; i++) {
      const lo = (h[2 * i + 1] >>> 0) + (v[2 * i + 1] >>> 0);
      h[2 * i + 1] = lo | 0;
      h[2 * i] = (h[2 * i] + v[2 * i] + Math.floor(lo / 4294967296)) | 0;
    }
  };

  const words32 = (list) => Int32Array.from(list.map((x) => Number(x >> 32n) | 0));
  const words64 = (list) => Int32Array.from(list.flatMap((x) => [Number(x >> 32n) | 0, Number(x & 0xffffffffn) | 0]));
  const algorithms = {
    md5: { block: 64, out: 16, little: true, init: () => Int32Array.of(0x67452301, 0xefcdab89 | 0, 0x98badcfe | 0, 0x10325476), run: md5Block },
    sha1: { block: 64, out: 20, init: () => Int32Array.of(0x67452301, 0xefcdab89 | 0, 0x98badcfe | 0, 0x10325476, 0xc3d2e1f0 | 0), run: sha1Block },
    sha256: { block: 64, out: 32, init: () => words32(initial64(8, 0)), run: sha256Block },
    sha224: { block: 64, out: 28, init: () => Int32Array.from(initial64(8, 8).map((x) => Number(x & 0xffffffffn) | 0)), run: sha256Block },
    sha512: { block: 128, out: 64, init: () => words64(initial64(8, 0)), run: sha512Block },
    sha384: { block: 128, out: 48, init: () => words64(initial64(8, 8)), run: sha512Block },
  };
  const aliases = { "sha-1": "sha1", "sha-256": "sha256", "sha-224": "sha224", "sha-384": "sha384", "sha-512": "sha512",
                    "rsa-md5": "md5", "rsa-sha1": "sha1", "rsa-sha256": "sha256", "rsa-sha384": "sha384",
                    "rsa-sha512": "sha512", "rsa-sha224": "sha224", "ssl3-md5": "md5", "ssl3-sha1": "sha1" };
  const algorithmOf = (name) => {
    const key = String(name).toLowerCase();
    return algorithms[key] ? key : aliases[key] in algorithms ? aliases[key] : null;
  };

  class Digest {
    constructor(name) {
      this.name = name;
      this.a = algorithms[name];
      this.h = this.a.init();
      this.buffer = new Uint8Array(this.a.block);
      this.have = 0;
      this.length = 0;
    }
    clone() {
      const copy = new Digest(this.name);
      copy.h = this.h.slice();
      copy.buffer = this.buffer.slice();
      copy.have = this.have;
      copy.length = this.length;
      return copy;
    }
    update(bytes) {
      const block = this.a.block;
      let at = 0;
      this.length += bytes.length;
      if (this.have) {
        const n = Math.min(block - this.have, bytes.length);
        this.buffer.set(bytes.subarray(0, n), this.have);
        this.have += n;
        at = n;
        if (this.have < block) {
          return;
        }
        this.a.run(this.h, this.buffer, 0);
        this.have = 0;
      }
      for (; at + block <= bytes.length; at += block) {
        this.a.run(this.h, bytes, at);
      }
      if (at < bytes.length) {
        this.buffer.set(bytes.subarray(at));
        this.have = bytes.length - at;
      }
    }
    digest() {
      const block = this.a.block;
      const lengthBytes = block === 64 ? 8 : 16;
      const bits = BigInt(this.length) * 8n;
      const pad = new Uint8Array(((this.have + 1 + lengthBytes + block - 1) & ~(block - 1)) - this.have);
      pad[0] = 0x80;
      for (let i = 0; i < 8; i++) {
        const byte = Number((bits >> BigInt(8 * i)) & 0xffn);
        if (this.a.little) {
          pad[pad.length - 8 + i] = byte;
        } else {
          pad[pad.length - 1 - i] = byte;
        }
      }
      this.length -= pad.length;	// (the padding is not part of the message)
      this.update(pad);
      const out = new Uint8Array(this.h.length * 4);
      const v = new DataView(out.buffer);
      for (let i = 0; i < this.h.length; i++) {
        v.setInt32(4 * i, this.h[i], !!this.a.little);
      }
      return out.subarray(0, this.a.out);
    }
  }
  const digestOf = (name, bytes) => {
    const d = new Digest(name);
    d.update(bytes);
    return d.digest();
  };
  const hmacStart = (name, key) => {
    const block = algorithms[name].block;
    if (key.length > block) {
      key = digestOf(name, key);
    }
    const inner = new Uint8Array(block).fill(0x36), outer = new Uint8Array(block).fill(0x5c);
    for (let i = 0; i < key.length; i++) {
      inner[i] ^= key[i];
      outer[i] ^= key[i];
    }
    const state = { inner: new Digest(name), outer: new Digest(name) };
    state.inner.update(inner);
    state.outer.update(outer);
    return state;
  };
  const hmacEnd = (state) => {
    state.outer.update(state.inner.digest());
    return state.outer.digest();
  };
  hn.digest = digestOf;

  const bytesOf = (value, encoding) => {
    if (typeof value === "string") {
      const names = data.bindings.string_decoder.encodings;
      const index = encoding === undefined || encoding === null ? 1 : names.indexOf(hn.normalizeEncoding(encoding));
      hn.internalBinding("buffer");	// (the encoders are there)
      return hn.encodeString(value, index < 0 ? 1 : index);
    }
    if (ArrayBuffer.isView(value)) {
      return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    }
    if (value && value.__secret) {
      return value.__secret;
    }
    return new Uint8Array(value);
  };
  const encodedOut = (bytes, encoding) => {
    const { Buffer } = hn.requireBuiltin("buffer");
    const buffer = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return encoding && encoding !== "buffer" ? buffer.toString(encoding) : buffer;
  };
  const unsupported = (what) => {
    const error = new Error("hostnode: " + what + " is not available (no OpenSSL here; see hostnode/js/binding/crypto.js)");
    error.code = "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM";
    return error;
  };

  const kCryptoJobAsync = shape.kCryptoJobAsync, kCryptoJobSync = shape.kCryptoJobSync;
  // A piece of work that lib/ either waits for (run() gives [error, result]) or is told of later (ondone).
  const job = (provider, work) => class extends hn.AsyncWrap {
    constructor(mode, ...args) {
      super(provider);
      this.__mode = mode;
      this.__args = args;
    }
    run() {
      let result, error;
      try {
        result = work(...this.__args);
      } catch (e) {
        error = e;
      }
      if (this.__mode === kCryptoJobSync) {
        return [error, result];
      }
      loop.refs++;
      loop.enqueue(() => {
        loop.refs--;
        hn.makeCallback(this, () => this.ondone(error, result));
      });
      return undefined;
    }
  };

  const random = (target) => {
    for (let at = 0; at < target.length; at += 65536) {
      web.crypto.getRandomValues(target.subarray(at, Math.min(target.length, at + 65536)));
    }
  };

  hn.binding("crypto", () => {
    class Hash {
      constructor(algorithm, outputLength) {
        if (algorithm instanceof Hash) {
          this.__d = algorithm.__d.clone();
          return;
        }
        const name = algorithmOf(algorithm);
        if (name === null) {
          throw new Error("Digest method not supported");
        }
        if (outputLength !== undefined && outputLength !== algorithms[name].out) {
          throw new Error("Digest method not supported");
        }
        this.__d = new Digest(name);
      }
      update(value, encoding) {
        this.__d.update(bytesOf(value, encoding));
        return true;
      }
      digest(encoding) {
        return encodedOut(this.__d.digest(), encoding);
      }
    }
    class Hmac {
      init(algorithm, key) {
        const name = algorithmOf(algorithm);
        if (name === null) {
          const { codes } = hn.requireBuiltin("internal/errors");
          throw new codes.ERR_CRYPTO_INVALID_DIGEST(algorithm);
        }
        this.__state = hmacStart(name, bytesOf(key));
      }
      update(value, encoding) {
        this.__state.inner.update(bytesOf(value, encoding));
        return true;
      }
      digest(encoding) {
        const out = this.__state ? hmacEnd(this.__state) : new Uint8Array(0);
        this.__state = null;
        return encodedOut(out, encoding);
      }
    }
    // A key that is just bytes; the other kinds need what is not here.
    class KeyObjectHandle {
      init(type, key) {
        if (type !== shape.kKeyTypeSecret) {
          throw unsupported("a public or private key object");
        }
        this.__secret = bytesOf(key).slice();
      }
      getSymmetricKeySize() {
        return this.__secret.length;
      }
      export() {
        return encodedOut(this.__secret.slice());
      }
      equals(other) {
        return this.__secret.length === other.__secret.length && this.__secret.every((b, i) => b === other.__secret[i]);
      }
    }
    // The settings of TLS connections (lib/_tls_common.js fills one in; binding/tls.js reads it).
    class SecureContext {
      constructor() {
        this.__settings = { ca: [], cert: [], key: [], ciphers: null, minVersion: null, maxVersion: null, rootCerts: false };
      }
      init(protocol, minVersion, maxVersion) {
        this.__settings.minVersion = minVersion;
        this.__settings.maxVersion = maxVersion;
      }
      addCACert(cert) {
        this.__settings.ca.push(cert);
      }
      addRootCerts() {
        this.__settings.rootCerts = true;
      }
      setCert(cert) {
        this.__settings.cert.push(cert);
      }
      setKey(key, passphrase) {
        this.__settings.key.push([key, passphrase]);
      }
      setCiphers(ciphers) {
        this.__settings.ciphers = ciphers;
      }
      setCipherSuites(suites) {
        this.__settings.suites = suites;
      }
      getMinProto() {
        return this.__settings.minVersion || 0x0303;
      }
      getMaxProto() {
        return this.__settings.maxVersion || 0x0304;
      }
      close() {}
    }
    for (const name of shape.SecureContext.prototype) {
      if (!(name in SecureContext.prototype) && name !== "_external") {
        SecureContext.prototype[name] = function () {};
      }
    }

    let webcrypto = null;
    const binding = {
      __webcrypto: () => webcrypto || (webcrypto = hn.webcrypto()),
      Hash, Hmac, KeyObjectHandle, SecureContext,
      kCryptoJobAsync, kCryptoJobSync,
      HashJob: job("HASHREQUEST", (algorithm, input, outputLength) => {
        const name = algorithmOf(algorithm);
        if (name === null) {
          throw new Error("Digest method not supported");
        }
        return digestOf(name, bytesOf(input)).slice().buffer;
      }),
      RandomBytesJob: job("RANDOMBYTESREQUEST", (buffer, offset, size) => {
        random(new Uint8Array(buffer instanceof ArrayBuffer || buffer instanceof SharedArrayBuffer ? buffer : buffer.buffer,
                              (buffer.byteOffset || 0) + (offset || 0), size));
      }),
      PBKDF2Job: job("PBKDF2REQUEST", (password, salt, iterations, length, algorithm) => {
        const name = algorithmOf(algorithm);
        if (name === null) {
          throw new Error("Digest method not supported");
        }
        const key = hmacStart(name, bytesOf(password));
        const saltBytes = bytesOf(salt);
        const out = new Uint8Array(length);
        const size = algorithms[name].out;
        for (let block = 1, at = 0; at < length; block++, at += size) {
          const first = { inner: key.inner.clone(), outer: key.outer.clone() };
          first.inner.update(saltBytes);
          first.inner.update(Uint8Array.of(block >>> 24, block >>> 16, block >>> 8, block));
          let u = hmacEnd(first);
          const t = u.slice();
          for (let i = 1; i < iterations; i++) {
            const next = { inner: key.inner.clone(), outer: key.outer.clone() };
            next.inner.update(u);
            u = hmacEnd(next);
            for (let k = 0; k < size; k++) {
              t[k] ^= u[k];
            }
          }
          out.set(t.subarray(0, Math.min(size, length - at)), at);
        }
        return out.buffer;
      }),
      secureBuffer: (size) => new Uint8Array(size),	// (node: bytes of the secure heap; lib/ indexes them)
      secureHeapUsed: () => undefined,
      timingSafeEqual(a, b) {
        const x = bytesOf(a), y = bytesOf(b);
        let diff = 0;
        for (let i = 0; i < x.length; i++) {
          diff |= x[i] ^ y[i];
        }
        return diff === 0;
      },
      getHashes: () => Object.keys(algorithms),
      getCachedAliases: () => {
        const cache = {};
        Object.keys(algorithms).forEach((name, i) => {
          cache[name] = i;
        });
        return cache;
      },
      getCiphers: () => [],
      getSSLCiphers: () => [],
      getCurves: () => [],
      getFipsCrypto: () => 0,
      setFipsCrypto() {},
      testFipsCrypto: () => false,
      getRootCertificates: () => [],
      // lib/internal/crypto/keys.js builds its key classes on one of ours.
      createNativeKeyObjectClass(callback) {
        class NativeKeyObject {
          constructor(handle) {
            this.handle = handle;
          }
        }
        return callback(NativeKeyObject);
      },
    };
    for (const name of Object.keys(shape)) {
      if (!(name in binding)) {
        const v = shape[name];
        if (v !== null && typeof v === "object" && v.$ === "function") {
          binding[name] = function () {
            throw unsupported("crypto: " + name);
          };
        } else if (v === null || typeof v !== "object") {
          binding[name] = v;
        }
      }
    }
    return binding;
  });

  // --- Web Cryptography (lib/internal/crypto/webcrypto.js in node): the browser's. Its answers come as
  // tasks of the browser's loop, so each open call counts as one of its operations (see loop.js).
  hn.webcrypto = () => {
    const real = web.crypto;
    const subtle = {};
    const methods = ["digest", "generateKey", "importKey", "exportKey", "sign", "verify", "encrypt", "decrypt",
                     "deriveBits", "deriveKey", "wrapKey", "unwrapKey"];
    for (const name of methods) {
      subtle[name] = (...args) => {
        loop.hostRef();
        const promise = real.subtle[name](...args);
        promise.then(() => loop.hostUnref(), () => loop.hostUnref());
        return promise;
      };
    }
    const crypto = {
      subtle,
      getRandomValues: (array) => real.getRandomValues(array),
      randomUUID: () => real.randomUUID(),
    };
    Object.defineProperty(crypto, Symbol.toStringTag, { value: "Crypto" });
    return { crypto, Crypto: Object, CryptoKey: globalThis.CryptoKey, SubtleCrypto: Object };
  };
})
