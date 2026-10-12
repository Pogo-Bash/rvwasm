// Bindings about bytes and text: buffer, string_decoder, encoding_binding,
// icu, url, blob, messaging. In node these are C++ on V8's strings, simdutf,
// ICU and ada; here they stand on what the browser has for the same jobs
// (TextDecoder, TextEncoder, URL, structuredClone, MessageChannel).
(function (hn) {
  "use strict";

  const { web, data } = hn;
  const shapes = data.bindings;
  const ASCII = 0, UTF8 = 1, BASE64 = 2, UCS2 = 3, LATIN1 = 4, HEX = 5, BUFFER = 6, BASE64URL = 7;

  // (ignoreBOM: a byte order mark stays in the text, as in node's own conversions.)
  const utf8 = new web.TextDecoder("utf-8", { ignoreBOM: true });
  const encoder = new web.TextEncoder();
  const fromCharCode = String.fromCharCode;

  const latin1Slice = (b, start, end) => {
    let s = "";
    for (let i = start; i < end; i += 8192) {
      s += fromCharCode.apply(null, b.subarray(i, Math.min(end, i + 8192)));
    }
    return s;
  };
  const asciiSlice = (b, start, end) => {
    let s = "";
    for (let i = start; i < end; i++) {
      s += fromCharCode(b[i] & 0x7f);
    }
    return s;
  };
  const utf8Slice = (b, start, end) => {
    const n = end - start;
    if (n < 24) {
      // Short and plain: quicker than asking the decoder.
      let s = "";
      let i = start;
      for (; i < end; i++) {
        const c = b[i];
        if (c > 127) {
          break;
        }
        s += fromCharCode(c);
      }
      if (i === end) {
        return s;
      }
    }
    return utf8.decode(b.subarray(start, end));
  };
  const hexDigits = "0123456789abcdef";
  const hexTable = [];
  for (let i = 0; i < 256; i++) {
    hexTable.push(hexDigits[i >> 4] + hexDigits[i & 15]);
  }
  const hexSlice = (b, start, end) => {
    let s = "";
    for (let i = start; i < end; i++) {
      s += hexTable[b[i]];
    }
    return s;
  };
  const ucs2Slice = (b, start, end) => {
    let s = "";
    const units = [];
    for (let i = start; i + 1 < end; i += 2) {
      units.push(b[i] | (b[i + 1] << 8));
      if (units.length === 4096) {
        s += fromCharCode.apply(null, units);
        units.length = 0;
      }
    }
    return s + fromCharCode.apply(null, units);
  };
  const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const base64Encode = (b, start, end, alphabet, pad) => {
    let s = "";
    let piece = "";
    let i = start;
    for (; i + 2 < end; i += 3) {
      const n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
      piece += alphabet[n >> 18] + alphabet[(n >> 12) & 63] + alphabet[(n >> 6) & 63] + alphabet[n & 63];
      if (piece.length >= 8192) {
        s += piece;
        piece = "";
      }
    }
    if (i + 1 === end) {
      const n = b[i] << 16;
      piece += alphabet[n >> 18] + alphabet[(n >> 12) & 63] + (pad ? "==" : "");
    } else if (i + 2 === end) {
      const n = (b[i] << 16) | (b[i + 1] << 8);
      piece += alphabet[n >> 18] + alphabet[(n >> 12) & 63] + alphabet[(n >> 6) & 63] + (pad ? "=" : "");
    }
    return s + piece;
  };
  // Either alphabet, anything else skipped, up to the first '=' (src/string_bytes.cc and base64.h do so).
  const b64Value = new Int8Array(128).fill(-1);
  for (let i = 0; i < 64; i++) {
    b64Value[B64.charCodeAt(i)] = i;
    b64Value[B64URL.charCodeAt(i)] = i;
  }
  const base64Decode = (string, out, offset, max) => {
    let bits = 0, have = 0, written = 0;
    for (let i = 0; i < string.length && written < max; i++) {
      const c = string.charCodeAt(i);
      if (c === 61) {
        break;
      }
      const v = c < 128 ? b64Value[c] : -1;
      if (v < 0) {
        continue;
      }
      bits = (bits << 6) | v;
      have += 6;
      if (have >= 8) {
        have -= 8;
        out[offset + written++] = (bits >> have) & 0xff;
      }
    }
    return written;
  };
  const base64Length = (string) => {
    let n = 0;
    for (let i = 0; i < string.length; i++) {
      const c = string.charCodeAt(i);
      if (c === 61) {
        break;
      }
      if (c < 128 && b64Value[c] >= 0) {
        n++;
      }
    }
    return Math.floor(n * 3 / 4);
  };
  const hexValue = (c) => (c >= 48 && c <= 57 ? c - 48 : c >= 97 && c <= 102 ? c - 87 : c >= 65 && c <= 70 ? c - 55 : -1);

  // The bytes of a string in an encoding (for searching and filling).
  const encode = (string, encoding) => {
    switch (encoding) {
    case UTF8: case BUFFER:
      return encoder.encode(string);
    case UCS2: {
      const out = new Uint8Array(string.length * 2);
      for (let i = 0; i < string.length; i++) {
        const c = string.charCodeAt(i);
        out[2 * i] = c & 0xff;
        out[2 * i + 1] = c >> 8;
      }
      return out;
    }
    case LATIN1: case ASCII: {
      const out = new Uint8Array(string.length);
      for (let i = 0; i < string.length; i++) {
        out[i] = string.charCodeAt(i) & 0xff;
      }
      return out;
    }
    case HEX: {
      const out = new Uint8Array(string.length >> 1);
      let n = 0;
      for (; n < out.length; n++) {
        const hi = hexValue(string.charCodeAt(2 * n)), lo = hexValue(string.charCodeAt(2 * n + 1));
        if (hi < 0 || lo < 0) {
          break;
        }
        out[n] = (hi << 4) | lo;
      }
      return out.subarray(0, n);
    }
    case BASE64: case BASE64URL: {
      const out = new Uint8Array(base64Length(string));
      return out.subarray(0, base64Decode(string, out, 0, out.length));
    }
    default:
      return encoder.encode(string);
    }
  };
  hn.encodeString = encode;

  const bytesOf = (v) => (v instanceof Uint8Array ? v :
    ArrayBuffer.isView(v) ? new Uint8Array(v.buffer, v.byteOffset, v.byteLength) : new Uint8Array(v));

  // src/node_buffer.cc: IndexOfOffset.
  const indexOfOffset = (length, offset, needleLength, forward) => {
    if (offset < 0) {
      if (offset + length >= 0) {
        return length + offset;
      }
      return forward || needleLength === 0 ? 0 : -1;
    }
    if (offset + needleLength <= length) {
      return offset;
    }
    if (needleLength === 0) {
      return length;
    }
    return forward ? -1 : length - 1;
  };
  const search = (haystack, needle, offset, forward, step) => {
    const hl = haystack.length, nl = needle.length;
    let at = indexOfOffset(hl, offset, nl, forward);
    if (nl === 0) {
      return at;
    }
    if (hl === 0 || at <= -1 || hl < nl) {
      return -1;
    }
    if (step === 2) {
      at &= ~1;
    }
    if (forward) {
      const first = needle[0];
      for (let i = at; i <= hl - nl; i += step) {
        if (step === 1) {
          i = haystack.indexOf(first, i);
          if (i < 0 || i > hl - nl) {
            return -1;
          }
        }
        let j = 0;
        while (j < nl && haystack[i + j] === needle[j]) {
          j++;
        }
        if (j === nl) {
          return i;
        }
      }
      return -1;
    }
    for (let i = Math.min(at, hl - nl); i >= 0; i -= step) {
      let j = 0;
      while (j < nl && haystack[i + j] === needle[j]) {
        j++;
      }
      if (j === nl) {
        return i;
      }
    }
    return -1;
  };

  hn.binding("buffer", () => {
    const binding = {
      kMaxLength: shapes.buffer.kMaxLength,
      kStringMaxLength: shapes.buffer.kStringMaxLength,
      byteLengthUtf8(string) {
        let n = 0;
        for (let i = 0; i < string.length; i++) {
          const c = string.charCodeAt(i);
          if (c < 0x80) {
            n += 1;
          } else if (c < 0x800) {
            n += 2;
          } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < string.length &&
                     (string.charCodeAt(i + 1) & 0xfc00) === 0xdc00) {
            n += 4;
            i++;
          } else {
            n += 3;
          }
        }
        return n;
      },
      copy(source, target, targetStart, sourceStart, nb) {
        const from = bytesOf(source);
        bytesOf(target).set(from.subarray(sourceStart, sourceStart + nb), targetStart);
        return nb;
      },
      compare(a, b) {
        a = bytesOf(a);
        b = bytesOf(b);
        const n = Math.min(a.length, b.length);
        for (let i = 0; i < n; i++) {
          if (a[i] !== b[i]) {
            return a[i] < b[i] ? -1 : 1;
          }
        }
        return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
      },
      compareOffset(source, target, targetStart, sourceStart, targetEnd, sourceEnd) {
        source = bytesOf(source);
        target = bytesOf(target);
        if (targetStart > target.length) {
          throw new RangeError('The value of "targetStart" is out of range.');
        }
        if (sourceStart > source.length) {
          throw new RangeError('The value of "sourceStart" is out of range.');
        }
        const sourceLength = Math.min(sourceEnd, source.length) - sourceStart;
        const targetLength = Math.min(targetEnd, target.length) - targetStart;
        const n = Math.min(sourceLength, targetLength);
        for (let i = 0; i < n; i++) {
          const x = source[sourceStart + i], y = target[targetStart + i];
          if (x !== y) {
            return x < y ? -1 : 1;
          }
        }
        return sourceLength < targetLength ? -1 : sourceLength > targetLength ? 1 : 0;
      },
      fill(buffer, value, start, end, encoding) {
        buffer = bytesOf(buffer);
        if (start > end || end > buffer.length) {
          return -2;
        }
        const length = end - start;
        let pattern;
        if (typeof value === "string") {
          const names = shapes.string_decoder.encodings;
          let enc = typeof encoding === "string" ? names.indexOf(hn.normalizeEncoding(encoding)) : UTF8;
          if (enc < 0) {
            enc = UTF8;
          }
          pattern = encode(value, enc);
          if (enc === UCS2 && value.length === 0) {
            return;
          }
        } else if (ArrayBuffer.isView(value)) {
          pattern = bytesOf(value);
        } else {
          buffer.fill((value >>> 0) & 255, start, end);
          return;
        }
        if (pattern.length >= length) {
          buffer.set(pattern.subarray(0, length), start);
          return;
        }
        if (pattern.length === 0) {
          return -1;
        }
        buffer.set(pattern, start);
        let done = pattern.length;
        while (done < length) {
          const n = Math.min(done, length - done);
          buffer.copyWithin(start + done, start, start + n);
          done += n;
        }
      },
      indexOfBuffer(haystack, needle, offset, encoding, forward) {
        return search(bytesOf(haystack), bytesOf(needle), offset, forward, encoding === UCS2 ? 2 : 1);
      },
      indexOfNumber(haystack, number, offset, forward) {
        haystack = bytesOf(haystack);
        const at = indexOfOffset(haystack.length, offset, 1, forward);
        if (at <= -1 || haystack.length === 0) {
          return -1;
        }
        return forward ? haystack.indexOf(number & 255, at) : haystack.lastIndexOf(number & 255, at);
      },
      indexOfString(haystack, string, offset, encoding, forward) {
        return search(bytesOf(haystack), encode(string, encoding), offset, forward, encoding === UCS2 ? 2 : 1);
      },
      swap16(buffer) {
        const b = bytesOf(buffer);
        for (let i = 0; i + 1 < b.length; i += 2) {
          const t = b[i];
          b[i] = b[i + 1];
          b[i + 1] = t;
        }
        return buffer;
      },
      swap32(buffer) {
        const b = bytesOf(buffer);
        for (let i = 0; i + 3 < b.length; i += 4) {
          let t = b[i];
          b[i] = b[i + 3];
          b[i + 3] = t;
          t = b[i + 1];
          b[i + 1] = b[i + 2];
          b[i + 2] = t;
        }
        return buffer;
      },
      swap64(buffer) {
        const b = bytesOf(buffer);
        for (let i = 0; i + 7 < b.length; i += 8) {
          for (let j = 0; j < 4; j++) {
            const t = b[i + j];
            b[i + j] = b[i + 7 - j];
            b[i + 7 - j] = t;
          }
        }
        return buffer;
      },
      isUtf8(input) {
        try {
          new web.TextDecoder("utf-8", { fatal: true }).decode(bytesOf(input));
          return true;
        } catch (error) {
          return false;
        }
      },
      isAscii(input) {
        const b = bytesOf(input);
        for (let i = 0; i < b.length; i++) {
          if (b[i] > 127) {
            return false;
          }
        }
        return true;
      },
      // -1: a character left over, -2: a character that is not of the alphabet (lib/buffer.js: atob).
      atob(input) {
        let bits = 0, have = 0, count = 0, out = "", pad = 0;
        for (let i = 0; i < input.length; i++) {
          const c = input.charCodeAt(i);
          if (c === 32 || c === 9 || c === 10 || c === 12 || c === 13) {
            continue;
          }
          if (c === 61) {
            pad++;
            continue;
          }
          const v = c < 128 && c !== 45 && c !== 95 ? b64Value[c] : -1;
          if (v < 0 || pad) {
            return -2;
          }
          bits = (bits << 6) | v;
          have += 6;
          count++;
          if (have >= 8) {
            have -= 8;
            out += fromCharCode((bits >> have) & 0xff);
          }
        }
        if (pad > 2 || (pad && (count + pad) % 4 !== 0)) {
          return -2;
        }
        return count % 4 === 1 ? -1 : out;
      },
      btoa(input) {
        const b = new Uint8Array(input.length);
        for (let i = 0; i < input.length; i++) {
          const c = input.charCodeAt(i);
          if (c > 255) {
            return -1;
          }
          b[i] = c;
        }
        return base64Encode(b, 0, b.length, B64, true);
      },
      // The contents move to a new buffer, which is the answer; the old one is empty from then on.
      detachArrayBuffer(buffer) {
        return typeof buffer.transfer === "function" ? buffer.transfer() : web.structuredClone(buffer, { transfer: [buffer] });
      },
      copyArrayBuffer(destination, destinationOffset, source, sourceOffset, length) {
        new Uint8Array(destination, destinationOffset, length).set(new Uint8Array(source, sourceOffset, length));
      },
      createUnsafeArrayBuffer: (size) => new ArrayBuffer(size),
      // (node's C++ makes buffers of its own with this; ours are made by lib/.)
      setBufferPrototype(prototype) {
        hn.bufferPrototype = prototype;
      },

      // These are put on Buffer.prototype by lib/internal/buffer.js: `this` is the buffer.
      asciiSlice(start, end) {
        return asciiSlice(this, start, end);
      },
      latin1Slice(start, end) {
        return latin1Slice(this, start, end);
      },
      utf8Slice(start, end) {
        return utf8Slice(this, start, end);
      },
      hexSlice(start, end) {
        return hexSlice(this, start, end);
      },
      ucs2Slice(start, end) {
        return ucs2Slice(this, start, end);
      },
      base64Slice(start, end) {
        return base64Encode(this, start, end, B64, true);
      },
      base64urlSlice(start, end) {
        return base64Encode(this, start, end, B64URL, false);
      },
      asciiWrite(string, offset = 0, length = this.length - offset) {
        const n = Math.min(length, string.length, this.length - offset);
        for (let i = 0; i < n; i++) {
          this[offset + i] = string.charCodeAt(i) & 0xff;
        }
        return n;
      },
      latin1Write(string, offset = 0, length = this.length - offset) {
        const n = Math.min(length, string.length, this.length - offset);
        for (let i = 0; i < n; i++) {
          this[offset + i] = string.charCodeAt(i) & 0xff;
        }
        return n;
      },
      utf8Write(string, offset = 0, length = this.length - offset) {
        length = Math.min(length, this.length - offset);
        return encoder.encodeInto(string, this.subarray(offset, offset + length)).written;
      },
      ucs2Write(string, offset = 0, length = this.length - offset) {
        const n = Math.min(length >> 1, string.length, (this.length - offset) >> 1);
        for (let i = 0; i < n; i++) {
          const c = string.charCodeAt(i);
          this[offset + 2 * i] = c & 0xff;
          this[offset + 2 * i + 1] = c >> 8;
        }
        return n * 2;
      },
      hexWrite(string, offset = 0, length = this.length - offset) {
        const n = Math.min(length, string.length >> 1, this.length - offset);
        let i = 0;
        for (; i < n; i++) {
          const hi = hexValue(string.charCodeAt(2 * i)), lo = hexValue(string.charCodeAt(2 * i + 1));
          if (hi < 0 || lo < 0) {
            break;
          }
          this[offset + i] = (hi << 4) | lo;
        }
        return i;
      },
      base64Write(string, offset = 0, length = this.length - offset) {
        return base64Decode(string, this, offset, Math.min(length, this.length - offset));
      },
      base64urlWrite(string, offset = 0, length = this.length - offset) {
        return base64Decode(string, this, offset, Math.min(length, this.length - offset));
      },
    };
    return binding;
  });
  hn.normalizeEncoding = (name) => {
    switch (String(name).toLowerCase()) {
    case "utf8": case "utf-8": return "utf8";
    case "ucs2": case "ucs-2": case "utf16le": case "utf-16le": return "utf16le";
    case "latin1": case "binary": return "latin1";
    case "base64": return "base64";
    case "base64url": return "base64url";
    case "ascii": return "ascii";
    case "hex": return "hex";
    default: return "utf8";
    }
  };

  // --- string_decoder (src/string_decoder.cc): text out of bytes that arrive in pieces. The state is a
  // buffer of lib/'s: what is left of a character that has not arrived whole, and counts.
  hn.binding("string_decoder", () => {
    const s = shapes.string_decoder;
    const kMissing = s.kMissingBytes, kBuffered = s.kBufferedBytes, kEncoding = s.kEncodingField;
    const whole = (encoding, b, start, end) => {
      switch (encoding) {
      case ASCII: return asciiSlice(b, start, end);
      case LATIN1: return latin1Slice(b, start, end);
      case HEX: return hexSlice(b, start, end);
      case UCS2: return ucs2Slice(b, start, end);
      case BASE64: return base64Encode(b, start, end, B64, true);
      case BASE64URL: return base64Encode(b, start, end, B64URL, false);
      default: return utf8.decode(b.subarray(start, end));
      }
    };
    // How many bytes at the end of b are the start of a character that is not all there.
    const utf8Tail = (b) => {
      const n = b.length;
      for (let back = 1; back <= Math.min(4, n); back++) {
        const c = b[n - back];
        if ((c & 0xc0) === 0x80) {
          continue;
        }
        const need = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 1;
        return need > back && c < 0xf5 ? [back, need - back] : [0, 0];
      }
      return [0, 0];
    };
    const decode = (state, buffer) => {
      const encoding = state[kEncoding];
      let b = bytesOf(buffer);
      if (encoding === ASCII || encoding === LATIN1 || encoding === HEX) {
        return whole(encoding, b, 0, b.length);
      }
      const kept = state[kBuffered];
      if (kept) {
        const joined = new Uint8Array(kept + b.length);
        joined.set(state.subarray(0, kept));
        joined.set(b, kept);
        b = joined;
        state[kBuffered] = 0;
        state[kMissing] = 0;
      }
      let keep = 0, missing = 0;
      if (encoding === UTF8) {
        [keep, missing] = utf8Tail(b);
      } else if (encoding === UCS2) {
        keep = b.length % 2;
        missing = keep ? 1 : 0;
        const last = b.length - keep - 2;
        if (last >= 0 && (b[last + 1] & 0xfc) === 0xd8) {
          // The first half of a pair: wait for the second.
          keep += 2;
          missing = keep === 3 ? 3 : 2;
        }
      } else {
        keep = b.length % 3;
        missing = keep ? 3 - keep : 0;
      }
      const end = b.length - keep;
      for (let i = 0; i < keep; i++) {
        state[i] = b[end + i];
      }
      state[kBuffered] = keep;
      state[kMissing] = missing;
      return end > 0 ? whole(encoding, b, 0, end) : "";
    };
    const flush = (state) => {
      const encoding = state[kEncoding];
      const kept = state[kBuffered];
      state[kBuffered] = 0;
      state[kMissing] = 0;
      if (!kept) {
        return "";
      }
      if (encoding === UTF8) {
        return "�";
      }
      if (encoding === UCS2) {
        return ucs2Slice(state, 0, kept & ~1);
      }
      return whole(encoding, state, 0, kept);
    };
    return { ...s, decode, flush };
  });

  // --- encoding_binding and icu: TextEncoder and TextDecoder, by the browser's.
  const decoders = new Map();
  const decoderFor = (encoding, fatal, ignoreBOM) => {
    const key = encoding + (fatal ? "|f" : "") + (ignoreBOM ? "|b" : "");
    let d = decoders.get(key);
    if (!d) {
      d = new web.TextDecoder(encoding, { fatal, ignoreBOM });
      decoders.set(key, d);
    }
    return d;
  };
  const invalidData = (encoding) => {
    const { codes } = hn.requireBuiltin("internal/errors");
    return new codes.ERR_ENCODING_INVALID_ENCODED_DATA(encoding, undefined);
  };
  const hostname = (domain) => {
    try {
      return new web.URL("ws://" + domain).hostname;
    } catch (error) {
      return "";
    }
  };
  const toUnicode = (domain) => {
    const ascii = hostname(domain);
    return ascii === "" ? "" : hn.requireBuiltin("punycode").toUnicode(ascii);
  };
  hn.binding("encoding_binding", () => {
    const results = new Uint32Array(2);
    return {
      encodeIntoResults: results,
      encodeInto(source, destination) {
        const r = encoder.encodeInto(source, destination);
        results[0] = r.read;
        results[1] = r.written;
      },
      encodeUtf8String: (string) => encoder.encode(string),
      decodeUTF8(buffer, ignoreBOM, fatal) {
        try {
          return decoderFor("utf-8", !!fatal, !!ignoreBOM).decode(
            ArrayBuffer.isView(buffer) || !(buffer instanceof SharedArrayBuffer) ? buffer : new Uint8Array(buffer).slice());
        } catch (error) {
          throw invalidData("utf-8");
        }
      },
      decodeLatin1(buffer, ignoreBOM, fatal) {
        const b = bytesOf(buffer);
        return latin1Slice(b, 0, b.length);
      },
      toASCII: hostname,
      toUnicode,
    };
  });
  hn.binding("icu", () => {
    // Columns a string takes on a terminal (lib/internal/util/inspect.js has this for a node without ICU).
    const fullWidth = (c) =>
      c >= 0x1100 && (
        c <= 0x115f || c === 0x2329 || c === 0x232a ||
        (c >= 0x2e80 && c <= 0x3247 && c !== 0x303f) ||
        (c >= 0x3250 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0xa4c6) || (c >= 0xa960 && c <= 0xa97c) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe19) ||
        (c >= 0xfe30 && c <= 0xfe6b) || (c >= 0xff01 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
        (c >= 0x1b000 && c <= 0x1b001) || (c >= 0x1f300 && c <= 0x1f64f) || (c >= 0x20000 && c <= 0x3fffd));
    const zeroWidth = (c) =>
      c <= 0x1f || (c >= 0x7f && c <= 0x9f) || (c >= 0x300 && c <= 0x36f) || (c >= 0x200b && c <= 0x200f) ||
      (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f) ||
      (c >= 0xe0100 && c <= 0xe01ef);
    return {
      getStringWidth(string, ambiguousAsFullWidth, expandEmojiSequence) {
        let width = 0;
        for (const char of String(string).normalize("NFC")) {
          const code = char.codePointAt(0);
          if (fullWidth(code)) {
            width += 2;
          } else if (!zeroWidth(code)) {
            width++;
          }
        }
        return width;
      },
      icuErrName: (code) => "U_ERROR_" + code,
      transcode(source, fromEncoding, toEncoding) {
        const names = shapes.string_decoder.encodings;
        const from = names.indexOf(hn.normalizeEncoding(fromEncoding));
        const to = names.indexOf(hn.normalizeEncoding(toEncoding));
        const b = bytesOf(source);
        const string = from === UCS2 ? ucs2Slice(b, 0, b.length) : from === UTF8 ? utf8.decode(b) :
          from === ASCII ? asciiSlice(b, 0, b.length) : latin1Slice(b, 0, b.length);
        let out;
        if (to === LATIN1 || to === ASCII) {
          out = new Uint8Array(string.length);
          for (let i = 0; i < string.length; i++) {
            const c = string.charCodeAt(i);
            out[i] = c > (to === ASCII ? 127 : 255) ? 63 : c;
          }
        } else {
          out = encode(string, to);
        }
        const { Buffer } = hn.requireBuiltin("buffer");
        return Buffer.from(out.buffer, out.byteOffset, out.byteLength);
      },
      hasConverter(name) {
        try {
          new web.TextDecoder(name);
          return true;
        } catch (error) {
          return false;
        }
      },
      // flags: 1 flush, 2 fatal, 4 ignore the byte order mark (lib/internal/encoding.js).
      getConverter(encoding, flags) {
        try {
          return { decoder: new web.TextDecoder(encoding, { fatal: !!(flags & 2), ignoreBOM: !!(flags & 4) }), encoding };
        } catch (error) {
          return undefined;
        }
      },
      decode(converter, input, flags, fromEncoding) {
        try {
          const b = input instanceof SharedArrayBuffer ? new Uint8Array(input).slice() : input;
          return converter.decoder.decode(b, { stream: !(flags & 1) });
        } catch (error) {
          throw invalidData(fromEncoding || converter.encoding);
        }
      },
    };
  });

  // --- url (src/node_url.cc, on the ada parser there): the browser's URL parses; what lib/internal/url.js
  // wants is the serialized address and where its parts begin and end in it.
  hn.binding("url", () => {
    const OMITTED = 0xffffffff;
    const components = new Uint32Array(9);
    const schemes = { "http:": 0, "https:": 2, "ws:": 3, "ftp:": 4, "wss:": 5, "file:": 6 };
    const describe = (u) => {
      const href = u.href;
      const protocolEnd = u.protocol.length;
      let usernameEnd, hostStart, hostEnd, port = OMITTED, pathnameStart;
      if (href.startsWith("//", protocolEnd)) {
        const userStart = protocolEnd + 2;
        if (u.username !== "" || u.password !== "") {
          usernameEnd = userStart + u.username.length;
          hostStart = usernameEnd + (u.password !== "" ? 1 + u.password.length : 0);	// at the '@'
          hostEnd = hostStart + 1 + u.hostname.length;
        } else {
          usernameEnd = hostStart = userStart;
          hostEnd = hostStart + u.hostname.length;
        }
        pathnameStart = hostEnd;
        if (u.port !== "") {
          port = Number(u.port);
          pathnameStart += 1 + u.port.length;
        }
      } else {
        usernameEnd = hostStart = hostEnd = pathnameStart = protocolEnd;
        if (href.startsWith("/.", protocolEnd) && u.pathname.startsWith("//")) {
          pathnameStart += 2;
        }
      }
      const pathEnd = pathnameStart + u.pathname.length;
      const searchStart = href[pathEnd] === "?" ? pathEnd : OMITTED;
      const hash = href.indexOf("#", pathEnd);
      components[0] = protocolEnd;
      components[1] = usernameEnd;
      components[2] = hostStart;
      components[3] = hostEnd;
      components[4] = port;
      components[5] = pathnameStart;
      components[6] = searchStart;
      components[7] = hash < 0 ? OMITTED : hash;
      components[8] = u.protocol in schemes ? schemes[u.protocol] : 1;
      return href;
    };
    const setters = ["protocol", "host", "hostname", "port", "username", "password", "pathname", "search",
                     "hash", "href"];
    return {
      urlComponents: components,
      parse(input, base, raise) {
        let u;
        try {
          u = base === undefined ? new web.URL(input) : new web.URL(input, base);
        } catch (error) {
          if (raise) {
            const { codes } = hn.requireBuiltin("internal/errors");
            throw new codes.ERR_INVALID_URL(input, base);
          }
          return undefined;
        }
        return describe(u);
      },
      canParse(input, base) {
        try {
          void (base === undefined ? new web.URL(input) : new web.URL(input, base));
          return true;
        } catch (error) {
          return false;
        }
      },
      update(href, action, value) {
        let u;
        try {
          u = new web.URL(href);
          u[setters[action]] = value;
        } catch (error) {
          return false;
        }
        return describe(u);
      },
      format(href, hash, unicode, search, auth) {
        const u = new web.URL(href);
        if (!hash) {
          u.hash = "";
        }
        if (!search) {
          u.search = "";
        }
        if (!auth) {
          u.username = "";
          u.password = "";
        }
        let out = u.href;
        if (unicode && u.hostname !== "") {
          const shown = toUnicode(u.hostname);
          if (shown !== "" && shown !== u.hostname) {
            out = out.replace(u.hostname, shown);
          }
        }
        return out;
      },
      getOrigin: (href) => new web.URL(href).origin,
      domainToASCII: hostname,
      domainToUnicode: toUnicode,
    };
  });

  // --- blob (src/node_blob.cc): bytes kept aside. A blob here is the list of its parts.
  hn.binding("blob", () => {
    const store = new Map();
    class BlobHandle {
      constructor(parts, length) {
        this.parts = parts;
        this.length = length;
      }
      getReader() {
        const parts = this.parts;
        let at = 0;
        return {
          // callback(status, buffer, last): 0 end of stream, 1 more to come (lib/internal/blob.js).
          pull(callback) {
            if (at >= parts.length) {
              hn.loop.enqueue(() => callback(0, undefined, undefined));
              return;
            }
            const part = parts[at++];
            hn.loop.enqueue(() => callback(1, part.buffer.slice(part.byteOffset, part.byteOffset + part.byteLength), false));
          },
        };
      }
      slice(start, end) {
        const out = [];
        let at = 0;
        for (const part of this.parts) {
          const a = Math.max(start, at), b = Math.min(end, at + part.length);
          if (a < b) {
            out.push(part.subarray(a - at, b - at));
          }
          at += part.length;
        }
        return new BlobHandle(out, Math.max(0, end - start));
      }
    }
    return hn.complete("blob", {
      createBlob(sources, length) {
        const parts = [];
        for (const source of sources) {
          if (source instanceof BlobHandle) {
            parts.push(...source.parts);
          } else {
            parts.push(bytesOf(source).slice());
          }
        }
        return new BlobHandle(parts, length);
      },
      storeDataObject(id, handle, length, type) {
        store.set(id, { handle, length, type });
      },
      getDataObject(id) {
        const entry = store.get(id);
        return entry ? [entry.handle, entry.length, entry.type] : undefined;
      },
      revokeObjectURL(id) {
        store.delete(id);
      },
      concat(views) {
        let total = 0;
        for (const view of views) {
          total += view.byteLength;
        }
        const out = new Uint8Array(total);
        let at = 0;
        for (const view of views) {
          out.set(bytesOf(view), at);
          at += view.byteLength;
        }
        return out.buffer;
      },
    });
  });

  // --- messaging (src/node_messaging.cc): message ports and structured cloning are the browser's. A port
  // of the browser's delivers on the browser's own loop, so an open one counts as something of the
  // browser's that we wait for (see loop.js).
  hn.binding("messaging", () => {
    const symbols = hn.internalBinding("symbols");
    class JSTransferable {}
    class MessagePort extends hn.HandleWrap {
      constructor(port) {
        super("MESSAGEPORT");
        if (!port) {
          throw new TypeError("Illegal constructor");
        }
        this.__port = port;
        this.__started = false;
        port.onmessage = (event) => {
          hn.loop.enqueue(() => hn.makeCallback(this, () => {
            hn.perContext.emitMessage.call(this, event.data, event.ports || [], "message");
          }));
        };
        port.onmessageerror = () => {};
      }
      postMessage(value, transfer) {
        const list = Array.isArray(transfer) ? transfer : transfer && transfer.transfer ? transfer.transfer : [];
        this.__port.postMessage(value, list.map((t) => (t instanceof MessagePort ? t.__port : t)));
      }
      start() {
        if (!this.__started) {
          this.__started = true;
          this.__port.start();
          this.__hostRef(true);
        }
      }
      __hostRef(on) {
        if (on !== !!this.__counted) {
          this.__counted = on;
          if (on) {
            hn.loop.hostRef();
          } else {
            hn.loop.hostUnref();
          }
        }
      }
      ref() {
        super.ref();
        if (this.__started && !this.__closed) {
          this.__hostRef(true);
        }
      }
      unref() {
        super.unref();
        this.__hostRef(false);
      }
      drain() {}
      __close() {
        this.__hostRef(false);
        this.__port.close();
      }
    }
    class MessageChannel {
      constructor() {
        const channel = new web.MessageChannel();
        this.port1 = new MessagePort(channel.port1);
        this.port2 = new MessagePort(channel.port2);
      }
    }
    return hn.complete("messaging", {
      MessageChannel,
      MessagePort,
      JSTransferable,
      DOMException: hn.perContext.DOMException,
      stopMessagePort(port) {
        port.__hostRef(false);
      },
      checkMessagePort: (port) => port instanceof MessagePort,
      drainMessagePort() {},
      receiveMessageOnPort: () => symbols.no_message_symbol,
      setDeserializerCreateObjectFunction() {},
      structuredClone(value, options) {
        return web.structuredClone(value, options);
      },
      broadcastChannel(name) {
        throw new Error("hostnode: BroadcastChannel is not implemented");
      },
    });
  });
})
