// http_parser (src/node_http_parser.cc): lib/_http_*.js on the llhttp parser.
// The parser is llhttp itself, as WebAssembly: the build that undici carries
// (and with it the guest's own node, for fetch), taken from there by
// hostnode/extract.js. One instance of it serves all parsers; its callbacks
// come here with the parser's address.
(function (hn) {
  "use strict";

  const { data } = hn;
  const shape = data.bindings.http_parser;
  const S = shape.HTTPParser;
  const HPE_OK = 0, HPE_PAUSED_UPGRADE = 22, HPE_USER = 24;
  const MAX_PAIRS = 32;	// headers handed over in one go (kMaxHeaderFieldsCount)

  const latin1 = (bytes) => {
    let s = "";
    for (let i = 0; i < bytes.length; i += 4096) {
      s += String.fromCharCode.apply(null, bytes.subarray(i, i + 4096));
    }
    return s;
  };

  let llhttp = null;
  const parsers = new Map();	// address of llhttp's parser -> ours
  const view = (at, length) => new Uint8Array(llhttp.memory.buffer, at, length);
  const cstring = (at) => {
    const m = new Uint8Array(llhttp.memory.buffer);
    let end = at;
    while (m[end]) {
      end++;
    }
    return latin1(m.subarray(at, end));
  };
  let reasons = null;
  // Stop the parser with an error of ours, "CODE:text".
  const userError = (p, text) => {
    if (!reasons.has(text)) {
      const raw = hn.encoder.encode(text);
      const at = llhttp.malloc(raw.length + 1);
      view(at, raw.length + 1).set(raw);
      view(at + raw.length, 1)[0] = 0;
      reasons.set(text, at);
    }
    llhttp.llhttp_set_error_reason(p, reasons.get(text));
    return HPE_USER;
  };
  const load = () => {
    const bytes = hn.readFile(hn.home + "/llhttp.wasm");
    if (bytes === null) {
      throw new Error("hostnode: no llhttp.wasm in " + hn.home + " (hostnode/extract.js takes it from the guest's undici)");
    }
    reasons = new Map();
    const guard = (fn) => (p, ...args) => {
      const parser = parsers.get(p);
      try {
        return fn(parser, p, ...args);
      } catch (error) {
        parser.__exception = { error };
        return userError(p, "HPE_JS_EXCEPTION:JS Exception");
      }
    };
    const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {
      env: {
        wasm_on_message_begin: guard((parser) => parser.__messageBegin()),
        wasm_on_url: guard((parser, p, at, length) => parser.__piece("url", at, length)),
        wasm_on_status: guard((parser, p, at, length) => parser.__piece("status", at, length)),
        wasm_on_header_field: guard((parser, p, at, length) => parser.__piece("field", at, length)),
        wasm_on_header_value: guard((parser, p, at, length) => parser.__piece("value", at, length)),
        wasm_on_headers_complete: guard((parser) => parser.__headersComplete()),
        wasm_on_body: guard((parser, p, at, length) => parser.__body(at, length)),
        wasm_on_message_complete: guard((parser) => parser.__messageComplete()),
      },
    });
    llhttp = instance.exports;
    if (llhttp._initialize) {
      llhttp._initialize();
    }
  };

  class ConnectionsList {
    constructor() {
      this.__all = new Set();
      this.__active = new Set();
    }
    all() {
      return [...this.__all];
    }
    idle() {
      return [...this.__all].filter((parser) => parser.__lastMessageStart === 0);
    }
    active() {
      return [...this.__active];
    }
    // Parsers whose request has been arriving for too long (milliseconds; 0: no limit).
    expired(headersTimeout, requestTimeout) {
      const now = hn.loop.now();
      const out = [];
      for (const parser of this.__active) {
        const started = parser.__lastMessageStart;
        if (started === 0) {
          continue;
        }
        if ((!parser.__headersCompleted && headersTimeout > 0 && now - started > headersTimeout) ||
            (requestTimeout > 0 && now - started > requestTimeout)) {
          out.push(parser);
          this.__active.delete(parser);
        }
      }
      return out;
    }
  }

  class HTTPParser extends hn.AsyncWrap {
    constructor() {
      super("HTTPINCOMINGMESSAGE");
      if (llhttp === null) {
        load();
      }
      this.__p = 0;
      this.__type = 0;
      this.__list = null;
      this.__lastMessageStart = 0;
      this.__reset();
    }
    __reset() {
      this.__url = "";
      this.__status = "";
      this.__fields = [];
      this.__values = [];
      this.__last = "";
      this.__flushed = false;
      this.__headersCompleted = false;
      this.__headerBytes = 0;
      this.__exception = null;
      this.__current = null;
      this.__pendingPause = false;
      this.__executing = false;
    }
    initialize(type, resource, maxHeaderSize, lenient, list) {
      if (this.__p && this.__type !== type) {
        parsers.delete(this.__p);
        llhttp.llhttp_free(this.__p);
        this.__p = 0;
      }
      if (!this.__p) {
        this.__p = llhttp.llhttp_alloc(type);
        parsers.set(this.__p, this);
      } else {
        llhttp.llhttp_reset(this.__p);
      }
      this.__type = type;
      this.__provider = type === S.REQUEST ? "HTTPINCOMINGMESSAGE" : "HTTPCLIENTREQUEST";
      this.asyncReset(resource);
      this.__reset();
      this.__maxHeaderSize = maxHeaderSize || hn.options["--max-http-header-size"];
      lenient |= 0;
      const p = this.__p;
      llhttp.llhttp_set_lenient_headers(p, lenient & S.kLenientHeaders ? 1 : 0);
      llhttp.llhttp_set_lenient_chunked_length(p, lenient & S.kLenientChunkedLength ? 1 : 0);
      llhttp.llhttp_set_lenient_keep_alive(p, lenient & S.kLenientKeepAlive ? 1 : 0);
      llhttp.llhttp_set_lenient_transfer_encoding(p, lenient & S.kLenientTransferEncoding ? 1 : 0);
      llhttp.llhttp_set_lenient_version(p, lenient & S.kLenientVersion ? 1 : 0);
      llhttp.llhttp_set_lenient_data_after_close(p, lenient & S.kLenientDataAfterClose ? 1 : 0);
      llhttp.llhttp_set_lenient_optional_lf_after_cr(p, lenient & S.kLenientOptionalLFAfterCR ? 1 : 0);
      llhttp.llhttp_set_lenient_optional_crlf_after_chunk(p, lenient & S.kLenientOptionalCRLFAfterChunk ? 1 : 0);
      llhttp.llhttp_set_lenient_optional_cr_before_lf(p, lenient & S.kLenientOptionalCRBeforeLF ? 1 : 0);
      llhttp.llhttp_set_lenient_spaces_after_chunk_size(p, lenient & S.kLenientSpacesAfterChunkSize ? 1 : 0);
      this.__leave();
      this.__list = list || null;
      if (this.__list) {
        this.__lastMessageStart = hn.loop.now() || 0.001;
        this.__list.__all.add(this);
        this.__list.__active.add(this);
      }
    }
    __leave() {
      if (this.__list) {
        this.__list.__all.delete(this);
        this.__list.__active.delete(this);
      }
    }

    // --- What llhttp reports while it parses.
    __messageBegin() {
      this.__url = "";
      this.__status = "";
      this.__fields = [];
      this.__values = [];
      this.__last = "";
      this.__flushed = false;
      this.__headersCompleted = false;
      this.__headerBytes = 0;
      if (this.__list) {
        this.__list.__all.delete(this);
        this.__list.__active.delete(this);
        this.__lastMessageStart = hn.loop.now() || 0.001;
        this.__list.__all.add(this);
        this.__list.__active.add(this);
      }
      const cb = this[S.kOnMessageBegin];
      if (typeof cb === "function") {
        cb.call(this);
      }
      return 0;
    }
    // A piece of the address, the status text, or a header's name or value: they arrive in parts.
    __piece(kind, at, length) {
      this.__headerBytes += length;
      if (this.__headerBytes >= this.__maxHeaderSize) {
        return userError(this.__p, "HPE_HEADER_OVERFLOW:Header overflow");
      }
      const text = latin1(view(at, length));
      if (kind === "url") {
        this.__url += text;
      } else if (kind === "status") {
        this.__status += text;
      } else if (kind === "field") {
        if (this.__last !== "field") {
          if (this.__fields.length === MAX_PAIRS) {
            this.__flush();
          }
          this.__fields.push(text);
        } else {
          this.__fields[this.__fields.length - 1] += text;
        }
      } else if (this.__last !== "value") {
        this.__values.push(text);
      } else {
        this.__values[this.__values.length - 1] += text;
      }
      this.__last = kind;
      return 0;
    }
    __headers() {
      const out = [];
      for (let i = 0; i < this.__fields.length; i++) {
        out.push(this.__fields[i], this.__values[i] === undefined ? "" : this.__values[i].replace(/[ \t]+$/, ""));
      }
      return out;
    }
    // More headers than are handed over at once: the ones so far go ahead (lib/_http_common.js: parserOnHeaders).
    __flush() {
      const cb = this[S.kOnHeaders];
      if (typeof cb === "function") {
        cb.call(this, this.__headers(), this.__url);
      }
      this.__url = "";
      this.__fields = [];
      this.__values = [];
      this.__flushed = true;
    }
    __headersComplete() {
      this.__headersCompleted = true;
      this.__headerBytes = 0;
      const cb = this[S.kOnHeadersComplete];
      if (typeof cb !== "function") {
        return 0;
      }
      const p = this.__p;
      let headers, url;
      if (this.__flushed) {
        this.__flush();
      } else {
        headers = this.__headers();
        if (this.__type === S.REQUEST) {
          url = this.__url;
        }
      }
      this.__fields = [];
      this.__values = [];
      const request = this.__type === S.REQUEST;
      const r = cb.call(this, llhttp.llhttp_get_http_major(p), llhttp.llhttp_get_http_minor(p), headers,
                        request ? llhttp.llhttp_get_method(p) : undefined, url,
                        request ? undefined : llhttp.llhttp_get_status_code(p), request ? undefined : this.__status,
                        !!llhttp.llhttp_get_upgrade(p), !!llhttp.llhttp_should_keep_alive(p));
      return Number(r) | 0;
    }
    __body(at, length) {
      if (length === 0) {
        return 0;
      }
      const cb = this[S.kOnBody];
      if (typeof cb === "function") {
        const { Buffer } = hn.requireBuiltin("buffer");
        cb.call(this, Buffer.from(view(at, length)));
      }
      return 0;
    }
    __messageComplete() {
      if (this.__list) {
        this.__list.__all.delete(this);
        this.__list.__active.delete(this);
        this.__lastMessageStart = 0;
        this.__list.__all.add(this);
      } else {
        this.__lastMessageStart = 0;
      }
      if (this.__fields.length) {
        this.__flush();	// headers after the body
      }
      const cb = this[S.kOnMessageComplete];
      if (typeof cb === "function") {
        cb.call(this);
      }
      return 0;
    }

    // --- What lib/ asks. src/node_http_parser.cc: Parser::Execute.
    __execute(bytes) {
      const p = this.__p;
      let err, nread = 0, at = 0;
      this.__exception = null;
      this.__executing = true;
      try {
        if (bytes === null) {
          err = llhttp.llhttp_finish(p);
        } else {
          at = llhttp.malloc(bytes.length || 1);
          view(at, bytes.length).set(bytes);
          this.__current = bytes;
          err = llhttp.llhttp_execute(p, at, bytes.length);
          nread = bytes.length;
          if (err !== HPE_OK) {
            nread = llhttp.llhttp_get_error_pos(p) - at;
            if (err === HPE_PAUSED_UPGRADE) {
              err = HPE_OK;
              llhttp.llhttp_resume_after_upgrade(p);
            }
          }
        }
      } finally {
        this.__executing = false;
        this.__current = null;
        if (at) {
          llhttp.free(at);
        }
      }
      if (this.__pendingPause) {
        this.__pendingPause = false;
        llhttp.llhttp_pause(p);
      }
      if (this.__exception) {
        const thrown = this.__exception.error;
        this.__exception = null;
        throw thrown;
      }
      if (!llhttp.llhttp_get_upgrade(p) && err !== HPE_OK) {
        const error = new Error("Parse Error");
        error.bytesParsed = nread;
        const reason = cstring(llhttp.llhttp_get_error_reason(p));
        if (err === HPE_USER) {
          const colon = reason.indexOf(":");
          error.code = reason.slice(0, colon);
          error.reason = reason.slice(colon + 1);
        } else {
          error.code = cstring(llhttp.llhttp_errno_name(err));
          error.reason = reason;
        }
        return error;
      }
      return bytes === null ? undefined : nread;
    }
    execute(buffer) {
      const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength);
      return this.__execute(bytes);
    }
    finish() {
      return this.__execute(null);
    }
    pause() {
      if (this.__executing) {
        this.__pendingPause = true;
      } else {
        llhttp.llhttp_pause(this.__p);
      }
    }
    resume() {
      if (this.__executing) {
        this.__pendingPause = false;
      } else {
        llhttp.llhttp_resume(this.__p);
      }
    }
    getCurrentBuffer() {
      const { Buffer } = hn.requireBuiltin("buffer");
      return Buffer.from(this.__current || []);
    }
    duration() {
      return this.__lastMessageStart === 0 ? 0 : hn.loop.now() - this.__lastMessageStart;
    }
    headersCompleted() {
      return this.__headersCompleted;
    }
    // Read from a stream directly, without lib/ in between (the server does this with its sockets).
    consume(stream) {
      this.__stream = stream;
      stream.__consumer = (bytes) => {
        const ret = this.__execute(bytes);
        const cb = this[S.kOnExecute];
        if (typeof cb === "function") {
          this.__current = bytes;
          try {
            cb.call(this, ret);
          } finally {
            this.__current = null;
          }
        }
      };
    }
    unconsume() {
      if (this.__stream) {
        this.__stream.__consumer = null;
        this.__stream = null;
      }
    }
    remove() {
      this.__leave();
    }
    close() {
      this.__leave();
      if (this.__p) {
        parsers.delete(this.__p);
        llhttp.llhttp_free(this.__p);
        this.__p = 0;
      }
    }
    free() {
      // (The parser is kept for the next message: lib/ has a pool of them.)
      this.__leave();
    }
  }
  for (const name of Object.keys(S)) {
    if (typeof S[name] === "number" && name !== "length") {
      HTTPParser[name] = S[name];
    }
  }
  for (const name of ["__p", "__type", "__list", "__lastMessageStart", "__url", "__status", "__fields", "__values",
                      "__last", "__flushed", "__headersCompleted", "__headerBytes", "__exception", "__current",
                      "__pendingPause", "__executing", "__maxHeaderSize", "__stream"]) {
    Object.defineProperty(HTTPParser.prototype, name, { writable: true, enumerable: false, value: null });
  }
  for (const name of ["__all", "__active"]) {
    Object.defineProperty(ConnectionsList.prototype, name, { writable: true, enumerable: false, value: null });
  }

  hn.binding("http_parser", () => ({ HTTPParser, ConnectionsList, methods: shape.methods, allMethods: shape.allMethods }));
})
