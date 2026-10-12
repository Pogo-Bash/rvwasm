// zlib (src/node_zlib.cc): lib/zlib.js on the deflate format. node links
// the zlib library for this; there is none to link here, so the format is
// done in JavaScript: inflate in full, deflate with fixed Huffman codes
// (valid for every reader, larger than what zlib makes). Brotli is not here.
//
// Both work on a stream in pieces, as zlib's inflate() and deflate() do:
// given some input and some room for output, they use what they can of
// both and say what is left.
(function (hn) {
  "use strict";

  const { data, loop } = hn;
  const Z = data.bindings.constants.zlib;
  const Z_OK = 0, Z_STREAM_END = 1, Z_NEED_DICT = 2, Z_DATA_ERROR = -3, Z_BUF_ERROR = -5;
  const Z_NO_FLUSH = 0, Z_FINISH = 4;
  const DEFLATE = 1, INFLATE = 2, GZIP = 3, GUNZIP = 4, DEFLATERAW = 5, INFLATERAW = 6, UNZIP = 7;

  // --- Checksums.
  const crcTable = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    crcTable[n] = c;
  }
  const crc32 = (bytes, start, end, crc) => {
    crc = ~crc;
    for (let i = start; i < end; i++) {
      crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
    }
    return ~crc >>> 0;
  };
  const adler32 = (bytes, start, end, adler) => {
    let a = adler & 0xffff, b = adler >>> 16;
    for (let i = start; i < end;) {
      const stop = Math.min(end, i + 3800);
      for (; i < stop; i++) {
        a += bytes[i];
        b += a;
      }
      a %= 65521;
      b %= 65521;
    }
    return ((b << 16) | a) >>> 0;
  };

  // --- Huffman tables for reading: for each code of up to 15 bits, symbol and length.
  // table[bits read, reversed, lowest `fast` bits] = symbol << 4 | length, or a slow search for longer codes.
  const buildTable = (lengths, count) => {
    const blCount = new Uint16Array(16);
    let max = 0;
    for (let i = 0; i < count; i++) {
      blCount[lengths[i]]++;
      if (lengths[i] > max) {
        max = lengths[i];
      }
    }
    blCount[0] = 0;
    const next = new Uint16Array(16);
    let code = 0;
    for (let bits = 1; bits <= 15; bits++) {
      code = (code + blCount[bits - 1]) << 1;
      next[bits] = code;
    }
    const size = 1 << max;
    const table = new Uint32Array(size || 1);
    for (let symbol = 0; symbol < count; symbol++) {
      const length = lengths[symbol];
      if (!length) {
        continue;
      }
      let c = next[length]++;
      // The stream has the code's bits most significant first; we read least significant first.
      let reversed = 0;
      for (let i = 0; i < length; i++) {
        reversed = (reversed << 1) | (c & 1);
        c >>= 1;
      }
      for (let at = reversed; at < size; at += 1 << length) {
        table[at] = (symbol << 4) | length;
      }
    }
    return { table, max };
  };
  const lengthBase = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  const lengthExtra = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  const distBase = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
  const distExtra = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
  const order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
  let fixedLit = null, fixedDist = null;
  const fixedTables = () => {
    if (!fixedLit) {
      const l = new Uint8Array(288);
      l.fill(8, 0, 144);
      l.fill(9, 144, 256);
      l.fill(7, 256, 280);
      l.fill(8, 280, 288);
      fixedLit = buildTable(l, 288);
      fixedDist = buildTable(new Uint8Array(30).fill(5), 30);
    }
  };

  // Not enough input for the step being taken: it is undone and taken again when more has come.
  const MORE = {};

  // --- Inflate.
  class Inflater {
    constructor(mode, dictionary) {
      this.mode = mode;
      this.dictionary = dictionary || null;
      this.window = new Uint8Array(65536);	// what was written last, twice the distance a match can reach back
      this.carry = new Uint8Array(0);	// input that came but could not be used yet
      this.reset();
    }
    reset() {
      this.state = this.mode === INFLATERAW ? "block" : "header";
      this.bitBuffer = 0;
      this.bitCount = 0;
      this.windowAt = 0;
      this.windowFlushed = 0;
      this.final = false;
      this.stored = 0;
      this.lit = null;
      this.dist = null;
      this.matchLength = 0;
      this.matchDistance = 0;
      this.check = this.wrapper === "gzip" ? 0 : 1;
      this.wrapper = this.mode === INFLATERAW ? "raw" : null;
      this.total = 0;
      this.message = "";
    }
    // Bits from the input, least significant first.
    bits(n) {
      while (this.bitCount < n) {
        if (this.inAt >= this.inEnd) {
          throw MORE;
        }
        this.bitBuffer |= this.input[this.inAt++] << this.bitCount;
        this.bitCount += 8;
      }
      const value = this.bitBuffer & ((1 << n) - 1);
      this.bitBuffer >>>= n;
      this.bitCount -= n;
      return value;
    }
    byte() {
      if (this.bitCount >= 8) {
        return this.bits(8);
      }
      if (this.inAt >= this.inEnd) {
        throw MORE;
      }
      return this.input[this.inAt++];
    }
    symbol(huffman) {
      const { table, max } = huffman;
      while (this.bitCount < max) {
        if (this.inAt >= this.inEnd) {
          // The last code of a stream may be shorter than the longest one.
          const entry = table[this.bitBuffer & ((1 << max) - 1)];
          const length = entry & 15;
          if (length && length <= this.bitCount) {
            this.bitBuffer >>>= length;
            this.bitCount -= length;
            return entry >>> 4;
          }
          throw MORE;
        }
        this.bitBuffer |= this.input[this.inAt++] << this.bitCount;
        this.bitCount += 8;
      }
      const entry = table[this.bitBuffer & ((1 << max) - 1)];
      const length = entry & 15;
      if (!length) {
        throw new Error("invalid code");
      }
      this.bitBuffer >>>= length;
      this.bitCount -= length;
      return entry >>> 4;
    }
    put(byte) {
      this.window[this.windowAt++] = byte;
    }
    // The header of a zlib or gzip stream.
    header() {
      let wrapper = this.mode === GUNZIP ? "gzip" : this.mode === INFLATE ? "zlib" : null;
      const first = this.byte(), second = this.byte();
      if (wrapper === null) {
        wrapper = first === 0x1f && second === 0x8b ? "gzip" : "zlib";	// (unzip: whichever it is)
      }
      if (wrapper === "gzip") {
        if (first !== 0x1f || second !== 0x8b) {
          throw new Error("incorrect header check");
        }
        if (this.byte() !== 8) {
          throw new Error("unknown compression method");
        }
        const flags = this.byte();
        for (let i = 0; i < 6; i++) {
          this.byte();
        }
        if (flags & 4) {
          let n = this.byte();
          n |= this.byte() << 8;
          while (n--) {
            this.byte();
          }
        }
        if (flags & 8) {
          while (this.byte() !== 0) {
          }
        }
        if (flags & 16) {
          while (this.byte() !== 0) {
          }
        }
        if (flags & 2) {
          this.byte();
          this.byte();
        }
        this.check = 0;
      } else {
        if ((first & 15) !== 8) {
          throw new Error(((first << 8) | second) % 31 ? "incorrect header check" : "unknown compression method");
        }
        if (((first << 8) | second) % 31) {
          throw new Error("incorrect header check");
        }
        if (second & 0x20) {
          for (let i = 0; i < 4; i++) {
            this.byte();
          }
          if (!this.dictionary) {
            this.needDictionary = true;
            throw new Error("Missing dictionary");
          }
          const d = this.dictionary.subarray(Math.max(0, this.dictionary.length - 32768));
          this.window.set(d, 0);
          this.windowAt = this.windowFlushed = d.length;
        }
        this.check = 1;
      }
      this.wrapper = wrapper;
    }
    blockHeader() {
      this.final = this.bits(1) === 1;
      const type = this.bits(2);
      if (type === 0) {
        this.bits(this.bitCount & 7);	// to the next whole byte
        const length = this.byte() | (this.byte() << 8);
        const inverse = this.byte() | (this.byte() << 8);
        if ((length ^ 0xffff) !== inverse) {
          throw new Error("invalid stored block lengths");
        }
        this.stored = length;
        return "stored";
      }
      if (type === 1) {
        fixedTables();
        this.lit = fixedLit;
        this.dist = fixedDist;
        return "codes";
      }
      if (type === 3) {
        throw new Error("invalid block type");
      }
      const literals = this.bits(5) + 257, distances = this.bits(5) + 1, codes = this.bits(4) + 4;
      const codeLengths = new Uint8Array(19);
      for (let i = 0; i < codes; i++) {
        codeLengths[order[i]] = this.bits(3);
      }
      const codeTable = buildTable(codeLengths, 19);
      const lengths = new Uint8Array(literals + distances);
      for (let i = 0; i < lengths.length;) {
        const symbol = this.symbol(codeTable);
        if (symbol < 16) {
          lengths[i++] = symbol;
          continue;
        }
        let repeat, value = 0;
        if (symbol === 16) {
          if (i === 0) {
            throw new Error("invalid bit length repeat");
          }
          value = lengths[i - 1];
          repeat = 3 + this.bits(2);
        } else if (symbol === 17) {
          repeat = 3 + this.bits(3);
        } else {
          repeat = 11 + this.bits(7);
        }
        if (i + repeat > lengths.length) {
          throw new Error("invalid bit length repeat");
        }
        while (repeat--) {
          lengths[i++] = value;
        }
      }
      if (lengths[256] === 0) {
        throw new Error("invalid code -- missing end-of-block");
      }
      this.lit = buildTable(lengths.subarray(0, literals), literals);
      this.dist = buildTable(lengths.subarray(literals), distances);
      return "codes";
    }
    trailer() {
      if (this.wrapper === "raw") {
        return;
      }
      this.bits(this.bitCount & 7);
      if (this.wrapper === "gzip") {
        let crc = 0, size = 0;
        for (let i = 0; i < 4; i++) {
          crc |= this.byte() << (8 * i);
        }
        for (let i = 0; i < 4; i++) {
          size |= this.byte() << (8 * i);
        }
        if ((crc >>> 0) !== this.check) {
          throw new Error("incorrect data check");
        }
        if ((size >>> 0) !== (this.total >>> 0)) {
          throw new Error("incorrect length check");
        }
      } else {
        let adler = 0;
        for (let i = 0; i < 4; i++) {
          adler = ((adler << 8) | this.byte()) >>> 0;
        }
        if (adler !== this.check) {
          throw new Error("incorrect data check");
        }
      }
    }
    // Hand what is in the window and not yet handed out to the caller; true if all of it fitted.
    drain() {
      const n = Math.min(this.windowAt - this.windowFlushed, this.outEnd - this.outAt);
      if (n > 0) {
        const piece = this.window.subarray(this.windowFlushed, this.windowFlushed + n);
        this.output.set(piece, this.outAt);
        this.check = this.wrapper === "gzip" ? crc32(piece, 0, n, this.check) :
          this.wrapper === "zlib" ? adler32(piece, 0, n, this.check) : this.check;
        this.outAt += n;
        this.windowFlushed += n;
        this.total += n;
      }
      if (this.windowFlushed < this.windowAt) {
        return false;
      }
      if (this.windowAt > 32768 + 16384) {
        // Keep the last 32768 bytes, which is as far back as a match can reach.
        this.window.copyWithin(0, this.windowAt - 32768, this.windowAt);
        this.windowAt = this.windowFlushed = 32768;
      }
      return true;
    }
    // One step of the stream; each either completes or throws MORE with nothing done.
    step() {
      switch (this.state) {
      case "header":
        this.header();
        this.state = "block";
        return;
      case "block":
        this.state = this.blockHeader();
        return;
      case "stored": {
        if (this.stored === 0) {
          this.state = this.final ? "trailer" : "block";
          return;
        }
        // (Whole bytes that were read ahead into the bit buffer come first.)
        if (this.bitCount >= 8) {
          this.put(this.bits(8));
          this.stored--;
          return;
        }
        if (this.inAt >= this.inEnd) {
          throw MORE;
        }
        const n = Math.min(this.stored, this.inEnd - this.inAt, this.window.length - this.windowAt);
        this.window.set(this.input.subarray(this.inAt, this.inAt + n), this.windowAt);
        this.windowAt += n;
        this.inAt += n;
        this.stored -= n;
        return;
      }
      case "codes": {
        const symbol = this.symbol(this.lit);
        if (symbol < 256) {
          this.put(symbol);
          return;
        }
        if (symbol === 256) {
          this.state = this.final ? "trailer" : "block";
          return;
        }
        if (symbol > 285) {
          throw new Error("invalid literal/length code");
        }
        const length = lengthBase[symbol - 257] + this.bits(lengthExtra[symbol - 257]);
        const code = this.symbol(this.dist);
        if (code > 29) {
          throw new Error("invalid distance code");
        }
        const distance = distBase[code] + this.bits(distExtra[code]);
        if (distance > this.windowAt) {
          throw new Error("invalid distance too far back");
        }
        this.matchLength = length;
        this.matchDistance = distance;
        this.state = "match";
        return;
      }
      case "match": {
        const w = this.window;
        let at = this.windowAt;
        const from = at - this.matchDistance;
        const n = Math.min(this.matchLength, w.length - at);
        for (let i = 0; i < n; i++) {
          w[at + i] = w[from + i];
        }
        this.windowAt = at + n;
        this.matchLength -= n;
        if (this.matchLength === 0) {
          this.state = "codes";
        }
        return;
      }
      case "trailer":
        this.trailer();
        this.state = "done";
        return;
      default:
        return;
      }
    }
    // Use input[inAt..inEnd) and fill output[outAt..outEnd). Returns a zlib status.
    run(input, inAt, inEnd, output, outAt, outEnd, flush) {
      if (this.carry.length) {
        const joined = new Uint8Array(this.carry.length + (inEnd - inAt));
        joined.set(this.carry);
        joined.set(input.subarray(inAt, inEnd), this.carry.length);
        this.input = joined;
        this.inAt = 0;
        this.inEnd = joined.length;
        this.carry = new Uint8Array(0);
      } else {
        this.input = input;
        this.inAt = inAt;
        this.inEnd = inEnd;
      }
      this.output = output;
      this.outAt = outAt;
      this.outEnd = outEnd;
      let status = Z_OK;
      try {
        for (;;) {
          // (The check at the end of a stream is of everything that has been handed out: all of it first.)
          if (this.window.length - this.windowAt < 1 || this.windowAt - this.windowFlushed > 32768 ||
              this.state === "trailer") {
            if (!this.drain()) {
              break;	// no room for output
            }
          }
          if (this.state === "done") {
            if (!this.drain()) {
              break;
            }
            status = Z_STREAM_END;
            break;
          }
          // Remember where this step began, to take it again if the input runs out in the middle.
          const at = this.inAt, buffer = this.bitBuffer, count = this.bitCount;
          try {
            this.step();
          } catch (error) {
            if (error !== MORE) {
              throw error;
            }
            this.inAt = at;
            this.bitBuffer = buffer;
            this.bitCount = count;
            this.drain();
            // What is left of the input is kept for the next time.
            this.carry = this.input.slice(this.inAt, this.inEnd);
            this.inAt = this.inEnd;
            if (flush === Z_FINISH && this.windowFlushed === this.windowAt) {
              status = Z_BUF_ERROR;
            }
            break;
          }
        }
      } catch (error) {
        this.message = error.message;
        status = this.needDictionary ? Z_NEED_DICT : Z_DATA_ERROR;
      }
      if (this.input === input) {
        this.availIn = this.inEnd - this.inAt;
      } else if (status === Z_STREAM_END) {
        // Past the end of the stream: what follows is the caller's, as much of it as is.
        this.availIn = Math.min(inEnd - inAt, this.inEnd - this.inAt);
      } else {
        // Input that was joined with what was carried counts as used; what is left of it is carried again.
        if (this.inAt < this.inEnd) {
          this.carry = this.input.slice(this.inAt, this.inEnd);
        }
        this.availIn = 0;
      }
      this.availOut = this.outEnd - this.outAt;
      this.input = null;
      this.output = null;
      return status;
    }
  }

  // --- Deflate: matches found with a table of where each three bytes were last seen, written with the
  // fixed codes of the format.
  class Deflater {
    constructor(mode, level) {
      this.mode = mode;
      this.level = level;
      this.reset();
    }
    reset() {
      this.pending = [];	// output made and not yet taken
      this.pendingAt = 0;
      this.out = new Uint8Array(65536);
      this.outAt = 0;
      this.bitBuffer = 0;
      this.bitCount = 0;
      this.started = false;
      this.finished = false;
      this.check = this.mode === GZIP ? 0 : 1;
      this.total = 0;
      this.history = new Uint8Array(0);	// the last 32768 bytes of input, for matches across calls
    }
    byte(b) {
      if (this.outAt === this.out.length) {
        this.pending.push(this.out);
        this.out = new Uint8Array(65536);
        this.outAt = 0;
      }
      this.out[this.outAt++] = b;
    }
    bits(value, n) {
      this.bitBuffer |= value << this.bitCount;
      this.bitCount += n;
      while (this.bitCount >= 8) {
        this.byte(this.bitBuffer & 0xff);
        this.bitBuffer >>>= 8;
        this.bitCount -= 8;
      }
    }
    // A Huffman code goes most significant bit first.
    code(value, n) {
      let reversed = 0;
      for (let i = 0; i < n; i++) {
        reversed = (reversed << 1) | ((value >> i) & 1);
      }
      this.bits(reversed, n);
    }
    literal(b) {
      if (b < 144) {
        this.code(0x30 + b, 8);
      } else {
        this.code(0x190 + b - 144, 9);
      }
    }
    match(length, distance) {
      let i = 28;
      while (lengthBase[i] > length) {
        i--;
      }
      const symbol = 257 + i;
      if (symbol < 280) {
        this.code(symbol - 256, 7);
      } else {
        this.code(0xc0 + symbol - 280, 8);
      }
      this.bits(length - lengthBase[i], lengthExtra[i]);
      let d = 29;
      while (distBase[d] > distance) {
        d--;
      }
      this.code(d, 5);
      this.bits(distance - distBase[d], distExtra[d]);
    }
    block(input, start, end, last) {
      this.bits(last ? 1 : 0, 1);
      if (this.level === 0) {
        // Stored, in pieces of at most 65535 bytes.
        this.bits(0, 2);
        if (this.bitCount) {
          this.bits(0, 8 - this.bitCount);
        }
        const n = end - start;
        this.byte(n & 0xff);
        this.byte(n >> 8);
        this.byte(~n & 0xff);
        this.byte((~n >> 8) & 0xff);
        for (let i = start; i < end; i++) {
          this.byte(input[i]);
        }
        return;
      }
      this.bits(1, 2);
      // The data to search is the history followed by this input.
      const base = this.history.length;
      const all = new Uint8Array(base + (end - start));
      all.set(this.history);
      all.set(input.subarray(start, end), base);
      const head = new Int32Array(32768).fill(-1);
      const hash = (i) => ((all[i] << 10) ^ (all[i + 1] << 5) ^ all[i + 2]) & 32767;
      for (let i = Math.max(0, base - 32768); i + 2 < base; i++) {
        head[hash(i)] = i;
      }
      let i = base;
      const limit = all.length;
      while (i < limit) {
        let best = 0, bestAt = 0;
        if (i + 2 < limit) {
          const h = hash(i);
          const candidate = head[h];
          head[h] = i;
          if (candidate >= 0 && i - candidate <= 32768) {
            let n = 0;
            const max = Math.min(258, limit - i);
            while (n < max && all[candidate + n] === all[i + n]) {
              n++;
            }
            if (n >= 3) {
              best = n;
              bestAt = candidate;
            }
          }
        }
        if (best) {
          this.match(best, i - bestAt);
          for (let k = 1; k < best && i + k + 2 < limit; k++) {
            head[hash(i + k)] = i + k;
          }
          i += best;
        } else {
          this.literal(all[i++]);
        }
      }
      this.code(0, 7);	// end of block
      this.history = all.subarray(Math.max(0, all.length - 32768)).slice();
    }
    run(input, inAt, inEnd, output, outAt, outEnd, flush) {
      if (!this.started) {
        this.started = true;
        if (this.mode === GZIP) {
          for (const b of [0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3]) {
            this.byte(b);
          }
        } else if (this.mode === DEFLATE) {
          this.byte(0x78);
          this.byte(0x9c);
        }
      }
      // (Called again only to take output that did not fit: nothing new to do for the same flush.)
      if (!this.finished && (inEnd > inAt || (flush !== Z_NO_FLUSH && !this.draining))) {
        if (this.mode === GZIP) {
          this.check = crc32(input, inAt, inEnd, this.check);
        } else if (this.mode === DEFLATE) {
          this.check = adler32(input, inAt, inEnd, this.check);
        }
        this.total += inEnd - inAt;
        const last = flush === Z_FINISH;
        const step = this.level === 0 ? 65535 : 1 << 20;
        let at = inAt;
        do {
          const stop = Math.min(inEnd, at + step);
          this.block(input, at, stop, last && stop === inEnd);
          at = stop;
        } while (at < inEnd);
        if (last) {
          if (this.bitCount) {
            this.bits(0, 8 - this.bitCount);
          }
          if (this.mode === GZIP) {
            for (let i = 0; i < 4; i++) {
              this.byte((this.check >>> (8 * i)) & 0xff);
            }
            for (let i = 0; i < 4; i++) {
              this.byte((this.total >>> (8 * i)) & 0xff);
            }
          } else if (this.mode === DEFLATE) {
            for (let i = 3; i >= 0; i--) {
              this.byte((this.check >>> (8 * i)) & 0xff);
            }
          }
          this.finished = true;
        } else if (flush !== Z_NO_FLUSH) {
          // An empty stored block: brings the output to a whole byte, as zlib's flushes do.
          this.bits(0, 3);
          if (this.bitCount) {
            this.bits(0, 8 - this.bitCount);
          }
          this.byte(0);
          this.byte(0);
          this.byte(0xff);
          this.byte(0xff);
        }
      }
      // Hand out what has been made.
      if (this.outAt) {
        this.pending.push(this.out.subarray(0, this.outAt));
        this.out = new Uint8Array(65536);
        this.outAt = 0;
      }
      let at = outAt;
      while (this.pending.length && at < outEnd) {
        const chunk = this.pending[0];
        const n = Math.min(chunk.length - this.pendingAt, outEnd - at);
        output.set(chunk.subarray(this.pendingAt, this.pendingAt + n), at);
        at += n;
        this.pendingAt += n;
        if (this.pendingAt === chunk.length) {
          this.pending.shift();
          this.pendingAt = 0;
        }
      }
      this.draining = this.pending.length > 0;
      this.availIn = 0;
      this.availOut = outEnd - at;
      return this.finished && !this.pending.length ? Z_STREAM_END : Z_OK;
    }
  }

  const codes = {};
  for (const name of ["Z_OK", "Z_STREAM_END", "Z_NEED_DICT", "Z_ERRNO", "Z_STREAM_ERROR", "Z_DATA_ERROR",
                      "Z_MEM_ERROR", "Z_BUF_ERROR", "Z_VERSION_ERROR"]) {
    codes[Z[name]] = name;
  }

  class Zlib extends hn.AsyncWrap {
    constructor(mode) {
      super("ZLIB");
      this.__mode = mode;
      this.__engine = null;
      this.__closed = false;
      this.__busy = false;
    }
    init(windowBits, level, memLevel, strategy, writeState, processCallback, dictionary) {
      this.__writeState = writeState;
      this.__callback = processCallback;
      const mode = this.__mode;
      this.__engine = mode === DEFLATE || mode === GZIP || mode === DEFLATERAW ?
        new Deflater(mode, level) :
        new Inflater(mode, dictionary ? new Uint8Array(dictionary.buffer, dictionary.byteOffset, dictionary.byteLength) : null);
    }
    params(level, strategy) {
      if (this.__engine instanceof Deflater) {
        this.__engine.level = level;
      }
    }
    reset() {
      this.__engine.reset();
    }
    close() {
      this.__closed = true;
      this.__engine = null;
    }
    // Returns false if an error was reported.
    __process(flush, input, inOffset, inLength, output, outOffset, outLength) {
      const engine = this.__engine;
      const inBytes = input ? new Uint8Array(input.buffer, input.byteOffset + inOffset, inLength) : new Uint8Array(0);
      const outBytes = new Uint8Array(output.buffer, output.byteOffset + outOffset, outLength);
      let status = engine.run(inBytes, 0, inBytes.length, outBytes, 0, outBytes.length, flush);
      // Several gzip streams one after the other are one (src/node_zlib.cc does the same).
      while (status === Z_STREAM_END && this.__mode === GUNZIP && engine.availIn > 0 &&
             inBytes[inBytes.length - engine.availIn] !== 0) {
        const left = engine.availIn;
        const produced = outBytes.length - engine.availOut;
        engine.reset();
        status = engine.run(inBytes, inBytes.length - left, inBytes.length, outBytes, produced, outBytes.length, flush);
      }
      this.__writeState[0] = engine.availOut;
      this.__writeState[1] = engine.availIn;
      if (status === Z_OK || status === Z_STREAM_END) {
        return true;
      }
      if (status === Z_BUF_ERROR) {
        if (flush !== Z_FINISH) {
          return true;
        }
        this.onerror("unexpected end of file", Z_BUF_ERROR, "Z_BUF_ERROR");
        return false;
      }
      this.onerror(status === Z_NEED_DICT ? "Missing dictionary" : engine.message || "invalid data", status, codes[status]);
      return false;
    }
    writeSync(flush, input, inOffset, inLength, output, outOffset, outLength) {
      this.__process(flush, input, inOffset, inLength, output, outOffset, outLength);
    }
    write(flush, input, inOffset, inLength, output, outOffset, outLength) {
      loop.refs++;
      loop.enqueue(() => {
        loop.refs--;
        if (this.__closed) {
          return;
        }
        hn.makeCallback(this, () => {
          if (this.__process(flush, input, inOffset, inLength, output, outOffset, outLength)) {
            this.__callback.call(this);
          }
        });
      });
    }
  }
  for (const name of ["__mode", "__engine", "__closed", "__busy", "__writeState", "__callback"]) {
    Object.defineProperty(Zlib.prototype, name, { writable: true, enumerable: false, value: null });
  }
  class Brotli extends hn.AsyncWrap {
    constructor() {
      super("ZLIB");
      const error = new Error("hostnode: Brotli compression is not implemented");
      error.code = "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM";
      throw error;
    }
  }

  hn.binding("zlib", () => ({
    Zlib,
    BrotliEncoder: Brotli,
    BrotliDecoder: Brotli,
    crc32(input, value) {
      const bytes = typeof input === "string" ? hn.encoder.encode(input) :
        new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      return crc32(bytes, 0, bytes.length, value >>> 0);
    },
    ZLIB_VERSION: data.bindings.zlib.ZLIB_VERSION,
  }));
})
