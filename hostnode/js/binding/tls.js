// tls_wrap (src/crypto/crypto_tls.cc): TLS over a stream. In node this is
// OpenSSL inside the process. Here it is the OpenSSL of the guest system
// beside the process: for every connection an `openssl s_client`, which does
// the handshake, checks the certificate against the system's authorities and
// the name, and encrypts.
//
//   the program  <-- plain text -->  openssl s_client  <-- TLS -->  the stream underneath
//                    (a socket pair)                    (a local socket, and us in between)
//
// The stream underneath is whatever lib/ gave us (a TCP connection it made,
// a tunnel through a proxy): s_client cannot be handed a connection, only an
// address, so it connects to a local socket of ours and we carry the bytes
// between that and the stream. What the program sees is a stream of plain
// text with the certificate checked, as with node; what it does not get is
// the certificate itself (only whose it is and that it was verified), session
// resumption, client side only, and HTTP/2 is never offered. A TLS written
// in JavaScript on the browser's cryptography would do away with the extra
// process; this needed nothing that was not already on the system.
(function (hn) {
  "use strict";

  const { loop, data, SYS, sys1, sys2, sys3, sys4, dv, u8, S } = hn;
  const shape = data.bindings.tls_wrap;
  const AF_UNIX = 1, SOCK_STREAM = 1, SOCK_NONBLOCK = 0o4000, SOCK_CLOEXEC = 0o2000000;
  const O_CLOEXEC = 0o2000000, O_NONBLOCK = 0o4000;
  const EAGAIN = 11, EINTR = 4;
  const SIGKILL = 9;
  const HIGH_WATER = 256 * 1024;

  hn.internalBinding("stream_wrap");
  const { WriteWrap } = hn.internalBinding("stream_wrap");
  hn.internalBinding("pipe_wrap");
  hn.internalBinding("process_wrap");

  // What OpenSSL says is wrong with a certificate, by its number, as node names it (src/crypto/crypto_common.cc).
  const verifyCodes = {
    2: "UNABLE_TO_GET_ISSUER_CERT", 3: "UNABLE_TO_GET_CRL", 4: "UNABLE_TO_DECRYPT_CERT_SIGNATURE",
    5: "UNABLE_TO_DECRYPT_CRL_SIGNATURE", 6: "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY", 7: "CERT_SIGNATURE_FAILURE",
    8: "CRL_SIGNATURE_FAILURE", 9: "CERT_NOT_YET_VALID", 10: "CERT_HAS_EXPIRED", 11: "CRL_NOT_YET_VALID",
    12: "CRL_HAS_EXPIRED", 13: "ERROR_IN_CERT_NOT_BEFORE_FIELD", 14: "ERROR_IN_CERT_NOT_AFTER_FIELD",
    15: "ERROR_IN_CRL_LAST_UPDATE_FIELD", 16: "ERROR_IN_CRL_NEXT_UPDATE_FIELD", 17: "OUT_OF_MEM",
    18: "DEPTH_ZERO_SELF_SIGNED_CERT", 19: "SELF_SIGNED_CERT_IN_CHAIN", 20: "UNABLE_TO_GET_ISSUER_CERT_LOCALLY",
    21: "UNABLE_TO_VERIFY_LEAF_SIGNATURE", 22: "CERT_CHAIN_TOO_LONG", 23: "CERT_REVOKED", 24: "INVALID_CA",
    25: "PATH_LENGTH_EXCEEDED", 26: "INVALID_PURPOSE", 27: "CERT_UNTRUSTED", 28: "CERT_REJECTED",
    62: "ERR_TLS_CERT_ALTNAME_INVALID", 64: "ERR_TLS_CERT_ALTNAME_INVALID",
  };

  let counter = 0;
  const temporary = () => (hn.env.get("TMPDIR") || "/tmp") + "/.hostnode-tls-" + hn.sys0(SYS.getpid) + "-" + (++counter);

  class TLSWrap extends hn.Stream {
    constructor(under, context, isServer) {
      super("TLSWRAP");
      // Whether the program is kept running is said to the stream underneath (lib/ passes ref and unref on
      // to it): this handle goes by that.
      this.__under = under;
      this.__follow(under);
      this.__context = context;
      this.__server = !!isServer;
      this.__servername = null;
      this.__alpn = null;
      this.__state = "new";	// new, handshake, open, failed, closed
      this.__summary = {};
      this.__verify = null;
      this.__held = [];	// writes the program made before the handshake was over
      this.__child = 0;
      this.__parts = [];	// what has to be closed: watchers and descriptors
      this.__errors = "";
    }

    // --- Settings, before the handshake.
    setServername(name) {
      this.__servername = String(name);
    }
    getServername() {
      return this.__servername || false;
    }
    setALPNProtocols(list) {
      // Lengths and names, as they go into the hello.
      const names = [];
      const bytes = new Uint8Array(list.buffer, list.byteOffset, list.byteLength);
      for (let at = 0; at < bytes.length; at += 1 + bytes[at]) {
        names.push(String.fromCharCode(...bytes.subarray(at + 1, at + 1 + bytes[at])));
      }
      // One only: s_client does not say which of several the server took. And not HTTP/2, which nothing
      // here could speak.
      this.__alpn = names.find((name) => name !== "h2") || null;
    }
    setVerifyMode() {}
    setSession() {}
    enableSessionCallbacks() {}
    enableKeylogCallback() {}
    enableTrace() {}
    enableCertCb() {}
    enableALPNCb() {}
    enablePskCallback() {}
    setPskIdentityHint() {}
    requestOCSP() {}
    setOCSPResponse() {}
    setKeyCert() {}
    setMaxSendFragment() {
      return true;
    }
    certCbDone() {}
    loadSession() {}
    newSessionDone() {}
    endParser() {}
    receive() {}
    writesIssuedByPrevListenerDone() {}

    // --- What the handshake came to.
    verifyError() {
      this.__asked = true;
      if (this.__held.length) {
        // The program may go on in spite of an error (rejectUnauthorized: false). If it does, we are still
        // here a moment from now, and what it wrote before can go out.
        loop.enqueue(() => this.__release());
      }
      return this.__verify;
    }
    getPeerCertificate() {
      if (this.__state !== "open") {
        return undefined;
      }
      // Not the certificate (s_client does not hand it over): whose it is, and the name it was checked for.
      const subject = {};
      for (const part of String(this.__summary["Peer certificate"] || "").split(/,\s*/)) {
        const eq = part.indexOf("=");
        if (eq > 0) {
          subject[part.slice(0, eq).trim()] = part.slice(eq + 1).trim();
        }
      }
      const checked = this.__checked;
      return {
        subject,
        issuer: {},
        subjectaltname: checked ? (checked.ip ? "IP Address:" : "DNS:") + checked.name : undefined,
        hostnode: "verified by the OpenSSL of the system; the certificate's other fields are not known here",
      };
    }
    getPeerX509Certificate() {
      return undefined;
    }
    getCertificate() {
      return undefined;
    }
    getX509Certificate() {
      return undefined;
    }
    getCipher() {
      const name = this.__summary.Ciphersuite;
      return name ? { name, standardName: name, version: this.__summary["Protocol version"] } : {};
    }
    getProtocol() {
      return this.__summary["Protocol version"] || null;
    }
    getALPNNegotiatedProtocol() {
      return this.__state === "open" && this.__alpn ? this.__alpn : false;
    }
    getEphemeralKeyInfo() {
      return {};
    }
    getSharedSigalgs() {
      return [];
    }
    isSessionReused() {
      return false;
    }
    getSession() {
      return undefined;
    }
    getTLSTicket() {
      return undefined;
    }
    getFinished() {
      return undefined;
    }
    getPeerFinished() {
      return undefined;
    }
    exportKeyingMaterial() {
      throw new Error("hostnode: exportKeyingMaterial is not available (TLS is done by a separate openssl)");
    }
    renegotiate() {
      throw new Error("hostnode: TLS renegotiation is not available");
    }

    // --- The handshake.
    start() {
      if (this.__state !== "new") {
        return;
      }
      this.__state = "handshake";
      this.__hold(true);
      if (this.__server) {
        this.__fail(new Error("hostnode: TLS servers are not implemented"));
        return;
      }
      const path = temporary();
      // The local socket s_client connects to.
      const listener = sys3(SYS.socket, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      const address = S + 1024;
      const raw = hn.encoder.encode(path);
      u8().fill(0, address, address + 110);
      dv().setUint16(address, AF_UNIX, true);
      u8().set(raw, address + 2);
      const old = sys1(SYS.umask, 0o077);
      const bound = listener < 0 ? listener : sys3(SYS.bind, listener, address, 2 + raw.length + 1);
      sys1(SYS.umask, old);
      if (bound < 0 || sys2(SYS.listen, listener, 1) < 0) {
        if (listener >= 0) {
          sys1(SYS.close, listener);
        }
        this.__fail(hn.uvException(bound < 0 ? bound : -22, "bind", path));
        return;
      }
      // Plain text to and from s_client, and what it has to say.
      if (sys4(SYS.socketpair, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, S) < 0) {
        sys1(SYS.close, listener);
        this.__fail(new Error("hostnode: socketpair failed"));
        return;
      }
      const plainOurs = dv().getInt32(S, true), plainTheirs = dv().getInt32(S + 4, true);
      sys2(SYS.pipe2, S, O_CLOEXEC);
      const errorsOurs = dv().getInt32(S, true), errorsTheirs = dv().getInt32(S + 4, true);

      const args = ["openssl", "s_client", "-brief", "-nocommands", "-ign_eof", "-unix", path];
      const settings = this.__context && this.__context.__settings;
      let peer = null;
      if (this.__servername) {
        args.push("-servername", this.__servername, "-verify_hostname", this.__servername);
        this.__checked = { name: this.__servername, ip: false };
      } else {
        args.push("-noservername");
        const out = {};
        if (this.__under.getpeername && this.__under.getpeername(out) === 0) {
          peer = out.address;
          args.push("-verify_ip", peer);
          this.__checked = { name: peer, ip: true };
        }
      }
      if (this.__alpn) {
        args.push("-alpn", this.__alpn);
      }
      if (settings && settings.ca.length && !settings.rootCerts) {
        // Authorities of the program's own instead of the system's.
        const file = path + ".ca";
        const fd = sys4(SYS.openat, -100, hn.path(file, hn.P0), 0o1101 | O_CLOEXEC, 0o600);
        if (fd >= 0) {
          for (const ca of settings.ca) {
            hn.writeAll(fd, typeof ca === "string" ? ca : new Uint8Array(ca.buffer, ca.byteOffset, ca.byteLength));
            hn.writeAll(fd, "\n");
          }
          sys1(SYS.close, fd);
          args.push("-CAfile", file, "-no-CApath");
          this.__files = [file];
        }
      }
      const env = [...hn.env].map(([k, v]) => k + "=" + v);
      const pid = hn.spawn({ file: "openssl", args, envPairs: env }, [plainTheirs, plainTheirs, errorsTheirs]);
      sys1(SYS.close, plainTheirs);
      sys1(SYS.close, errorsTheirs);
      if (pid < 0) {
        for (const fd of [listener, plainOurs, errorsOurs]) {
          sys1(SYS.close, fd);
        }
        sys3(SYS.unlinkat, -100, hn.path(path, hn.P0), 0);
        const error = hn.uvException(pid, "spawn openssl");
        error.message = "hostnode: TLS needs the openssl program of the system, which could not be started (" + error.message + ")";
        this.__fail(error);
        return;
      }
      this.__child = pid;
      this.__plain = plainOurs;
      hn.setNonBlocking(plainOurs, true);
      hn.setNonBlocking(errorsOurs, true);

      // s_client connects: from then on, bytes are carried between it and the stream underneath.
      const accept = loop.watch(listener, 1, () => {
        const fd = sys4(SYS.accept4, listener, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd === -EAGAIN || fd === -EINTR) {
          return;
        }
        accept.close();
        sys1(SYS.close, listener);
        sys3(SYS.unlinkat, -100, hn.path(path, hn.P0), 0);
        this.__parts = this.__parts.filter((part) => part !== closeListener);
        if (fd >= 0) {
          this.__carry(fd);
        }
      });
      const closeListener = () => {
        accept.close();
        sys1(SYS.close, listener);
        sys3(SYS.unlinkat, -100, hn.path(path, hn.P0), 0);
      };
      this.__parts.push(closeListener);

      // What s_client says: how the handshake went, or why it did not.
      const said = loop.watch(errorsOurs, 1, () => {
        const n = sys3(SYS.read, errorsOurs, S, 4096);
        if (n === -EAGAIN || n === -EINTR) {
          return;
        }
        if (n <= 0) {
          said.close();
          sys1(SYS.close, errorsOurs);
          this.__parts = this.__parts.filter((part) => part !== closeErrors);
          if (this.__state === "handshake") {
            this.__fail(this.__problem());
          }
          return;
        }
        this.__errors += hn.text(S, n);
        this.__heard();
      });
      const closeErrors = () => {
        said.close();
        sys1(SYS.close, errorsOurs);
      };
      this.__parts.push(closeErrors);
    }
    // While the handshake goes on, nothing else may be keeping the program running: we do.
    __hold(on) {
      if (on !== !!this.__holding) {
        this.__holding = on;
        this.__setActive(on || this.__fd >= 0 && (this.__reading || this.__writes.length > 0));
      }
    }
    // Go through what s_client has said so far.
    __heard() {
      if (this.__state !== "handshake") {
        return;
      }
      const lines = this.__errors.split("\n");
      for (const line of lines.slice(0, -1)) {
        const colon = line.indexOf(": ");
        if (colon > 0 && !/^[0-9A-F]{8,}:error:/.test(line)) {
          this.__summary[line.slice(0, colon)] = line.slice(colon + 2).trim();
        }
        const m = /^verify error:num=(\d+):(.*)$/.exec(line);
        if (m && !this.__firstVerifyError) {
          this.__firstVerifyError = [Number(m[1]), m[2]];
        }
      }
      if (!("CONNECTION ESTABLISHED" in this.__summary) && !lines.includes("CONNECTION ESTABLISHED")) {
        return;
      }
      if ("Verification" in this.__summary) {
        this.__verify = null;
      } else if ("Verification error" in this.__summary) {
        const [number, reason] = this.__firstVerifyError || [0, this.__summary["Verification error"]];
        const error = new Error(reason);
        error.code = verifyCodes[number] || "CERT_UNTRUSTED";
        this.__verify = error;
      } else {
        return;	// not all said yet
      }
      // Established. From here on our descriptor is the plain text.
      this.__state = "open";
      this.__hold(false);
      this.__adopt(this.__plain, true);
      if (this.__wantsReading) {
        hn.Stream.prototype.readStart.call(this);
      }
      if (this.__verify === null) {
        this.__release();
      }
      hn.makeCallback(this, () => this.onhandshakedone());
    }
    // Why the handshake failed, from what s_client said.
    __problem() {
      const text = this.__errors;
      let m = /connect:errno=(\d+)/.exec(text);
      if (m) {
        return hn.uvException(-Number(m[1]), "connect");
      }
      m = /^verify error:num=(\d+):(.*)$/m.exec(text);
      if (m) {
        const error = new Error(m[2]);
        error.code = verifyCodes[Number(m[1])] || "CERT_UNTRUSTED";
        return error;
      }
      m = /:error:[0-9A-F]+:SSL routines:[^:]*:([^:]+):/.exec(text);
      const error = new Error(m ? m[1] : "TLS handshake failed" + (text ? ": " + text.trim().split("\n").pop() : ""));
      error.code = m ? "ERR_SSL_" + m[1].toUpperCase().replace(/[^A-Z0-9]+/g, "_") : "ECONNRESET";
      return error;
    }
    __fail(error) {
      this.__state = "failed";
      loop.enqueue(() => {
        this.__hold(false);
        if (!this.__gone) {
          hn.makeCallback(this, () => this.onerror(error));
        }
      });
    }
    // Carry bytes between the stream underneath and s_client's side of the TLS.
    __carry(fd) {
      const under = this.__under;
      const inner = new hn.Pipe(0);
      inner.__follow(under);
      inner.__adopt(fd, true);
      this.__inner = inner;
      const request = () => {
        const r = new WriteWrap();
        r.oncomplete = () => {
          // Room again: read on from whichever side was waiting for it.
          if (inner.writeQueueSize < HIGH_WATER && !under.__closed) {
            under.__pause(false);
          }
          if (under.writeQueueSize < HIGH_WATER && !inner.__closed) {
            inner.__pause(false);
          }
        };
        return r;
      };
      // From the network to s_client.
      under.__consumer = (bytes) => {
        if (inner.__closed) {
          return;
        }
        inner.__write(request(), bytes);
        if (inner.writeQueueSize > HIGH_WATER) {
          under.__pause(true);
        }
      };
      under.onread = () => {
        // The network side has ended: s_client sees the same, and says so on the plain side.
        if (!inner.__closed && inner.__fd >= 0) {
          sys2(SYS.shutdown, inner.__fd, 1);
        }
      };
      under.readStart();
      // From s_client to the network.
      inner.onread = (buffer) => {
        if (buffer === undefined) {
          return;
        }
        if (under.__closed) {
          return;
        }
        under.__write(request(), new Uint8Array(buffer));
        if (under.writeQueueSize > HIGH_WATER) {
          inner.__pause(true);
        }
      };
      inner.readStart();
    }

    // --- The plain side: a stream like any other (binding/stream.js), once there is one.
    readStart() {
      this.__wantsReading = true;
      return this.__state === "open" ? super.readStart() : 0;
    }
    readStop() {
      this.__wantsReading = false;
      return this.__state === "open" ? super.readStop() : 0;
    }
    __write(request, bytes) {
      if (this.__state === "open" && (this.__verify === null || this.__asked) && !this.__held.length) {
        return super.__write(request, bytes);
      }
      // Nothing of the program's goes out before the certificate has been checked.
      const state = hn.internalBinding("stream_wrap").streamBaseState;
      state[2] = bytes.length;
      state[3] = 1;
      this.__held.push([request, bytes.slice()]);
      return 0;
    }
    __release() {
      if (this.__gone || this.__state !== "open") {
        return;
      }
      const held = this.__held;
      this.__held = [];
      const state = hn.internalBinding("stream_wrap").streamBaseState;
      for (const [request, bytes] of held) {
        const r = super.__write(request, bytes);
        if (r !== 0 || !state[3]) {
          // Written at once (or not at all): the program is still waiting to be told.
          loop.enqueue(() => hn.makeCallback(request, () => hn.oncomplete(request, r)));
        }
      }
    }
    // lib/ closes the stream underneath itself and tells us with this.
    destroySSL() {
      if (this.__gone) {
        return;
      }
      this.__gone = true;
      this.__state = "closed";
      this.__holding = false;
      this.__active = false;
      this.__closed = true;
      this.__recount();
      for (const [request] of this.__held.splice(0)) {
        loop.enqueue(() => hn.makeCallback(request, () => hn.oncomplete(request, -125)));
      }
      for (const part of this.__parts.splice(0)) {
        part();
      }
      if (this.__inner) {
        this.__inner.close();
      }
      if (this.__under) {
        this.__under.__consumer = null;
      }
      if (this.__fd >= 0) {
        hn.Stream.prototype.__close.call(this);
      } else if (this.__plain !== undefined && this.__plain !== null) {
        sys1(SYS.close, this.__plain);	// (the handshake never got as far as making it ours)
      }
      this.__plain = undefined;
      if (this.__child) {
        const pid = this.__child;
        this.__child = 0;
        sys2(SYS.kill, pid, SIGKILL);
        for (;;) {
          const r = sys4(SYS.wait4, pid, 0, 0, 0);
          if (r !== -EINTR) {
            break;
          }
        }
      }
      for (const file of this.__files || []) {
        sys3(SYS.unlinkat, -100, hn.path(file, hn.P0), 0);
      }
    }
  }
  for (const name of ["__under", "__context", "__server", "__servername", "__alpn", "__state", "__summary", "__verify",
                      "__held", "__child", "__parts", "__errors", "__plain", "__inner", "__checked", "__asked",
                      "__gone", "__files", "__wantsReading", "__firstVerifyError", "__holding"]) {
    Object.defineProperty(TLSWrap.prototype, name, { writable: true, enumerable: false, value: undefined });
  }

  hn.binding("tls_wrap", () => ({
    TLSWrap,
    wrap: (handle, context, isServer) => new TLSWrap(handle, context, isServer),
    HAVE_SSL_TRACE: shape.HAVE_SSL_TRACE,
  }));
})
