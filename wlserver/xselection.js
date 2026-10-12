// The clipboard between X programs and Wayland ones (and through data.js's
// bridge, the host's): X's CLIPBOARD and PRIMARY selections as the Wayland
// clipboard and primary selection, both ways. Text first: any type is passed
// on, but what X calls UTF8_STRING, STRING and TEXT is matched with the
// Wayland text types (text/plain;charset=utf-8, text/plain), and other X
// targets whose names are MIME types (with a slash) pass as those.
//
// X to Wayland: XFIXES tells us when an X program takes a selection
// (SelectSelectionInput on our own window). We ask the owner for its TARGETS
// (ConvertSelection to a property of our window) and make that the Wayland
// selection, with us as its source: when a Wayland client (or the host
// bridge) receives a type, we convert the selection to its target, read the
// property (INCR, the X way of passing big data in pieces, followed), and
// write the bytes into the client's descriptor. One conversion at a time per
// selection; others wait their turn.
//
// Wayland to X: when the Wayland selection changes to a Wayland client's (or
// host text), our window takes the X selection (SetSelectionOwner). An X
// program's SelectionRequest is answered: TARGETS with the types there are,
// TIMESTAMP, or the data, which we get from the Wayland source through a pipe
// (wl_data_source.send) and put on the requestor's property (in pieces
// appended to one property if it is long: no INCR on this side), then
// SelectionNotify. MULTIPLE is refused.
//
// Not done: XDND (drag and drop between X and Wayland programs).
(function (ws) {
  "use strict";

  const { log, sys1, sys2, sys3, SYS } = ws;
  const X = ws.X11;
  const { EV, MASK } = X;
  const O_NONBLOCK = 0o4000, O_CLOEXEC = 0o2000000, F_SETFL = 4, EAGAIN = 11, EINTR = 4;
  const MAX = 16 << 20;		// bytes of one selection at most
  const PIECE = 128 * 1024;	// bytes per ChangeProperty when we answer

  let conn = null, root = 0, A = null, win = 0, xfixes = null;
  const kinds = {
    clipboard: { atom: 0, prop: 0, queue: [], busy: null, owned: null },
    primary: { atom: 0, prop: 0, queue: [], busy: null, owned: null },
  };
  const kindOf = (atom) => atom === kinds.clipboard.atom ? "clipboard" : atom === kinds.primary.atom ? "primary" : null;

  // Atoms and MIME types, both ways (what we have interned so far).
  const atomOf = new Map(), nameOf = new Map();
  const TEXT_MIMES = ["text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"];

  // The Wayland type for an X target, or null for targets that are not data.
  const mimeFor = (name) => {
    if (name === "UTF8_STRING") {
      return "text/plain;charset=utf-8";
    }
    if (name === "STRING" || name === "TEXT") {
      return "text/plain";
    }
    return name && name.includes("/") ? name : null;
  };
  // The X targets for a set of Wayland types.
  const targetsFor = (mimes) => {
    const names = new Set();
    for (const m of mimes) {
      if (TEXT_MIMES.includes(m)) {
        names.add("UTF8_STRING");
        names.add("STRING");
        names.add("TEXT");
        if (m.includes("/")) {
          names.add(m);
        }
      } else if (m.includes("/")) {
        names.add(m);
      }
    }
    return [...names];
  };
  // The Wayland type to read for an X target asked of us.
  const pickMime = (mimes, target) => {
    if (target === "UTF8_STRING" || target === "STRING" || target === "TEXT") {
      return TEXT_MIMES.find((m) => mimes.includes(m)) || null;
    }
    return mimes.includes(target) ? target : null;
  };

  // Latin-1 (STRING) and UTF-8, both ways; what Latin-1 has no letter for becomes "?".
  const latin1ToUtf8 = (bytes) => {
    const out = new Uint8Array(2 * bytes.length);
    let n = 0;
    for (const c of bytes) {
      if (c < 0x80) {
        out[n++] = c;
      } else {
        out[n++] = 0xc0 | c >> 6;
        out[n++] = 0x80 | c & 0x3f;
      }
    }
    return out.subarray(0, n);
  };
  const utf8ToLatin1 = (bytes) => {
    const text = new TextDecoder().decode(bytes);
    const out = new Uint8Array(text.length);
    let n = 0;
    for (const ch of text) {
      const c = ch.codePointAt(0);
      out[n++] = c < 0x100 ? c : 0x3f;
    }
    return out.subarray(0, n);
  };

  const intern = (names, done) => {
    const missing = names.filter((n) => !atomOf.has(n));
    if (!missing.length) {
      done();
      return;
    }
    conn.internAtoms(missing, (atoms) => {
      for (const n of missing) {
        atomOf.set(n, atoms[n]);
        nameOf.set(atoms[n], n);
      }
      done();
    });
  };

  const atomName = (atom, done) => {
    if (nameOf.has(atom)) {
      done(nameOf.get(atom));
      return;
    }
    conn.getAtomName(atom, (name) => {
      if (name !== null) {
        nameOf.set(atom, name);
        atomOf.set(name, atom);
      }
      done(name);
    });
  };

  // ---- start and end (xwm.js)

  ws.xselectionStart = (c, r, atoms) => {
    conn = c;
    root = r;
    A = atoms;
    for (const name of Object.keys(atoms)) {
      atomOf.set(name, atoms[name]);
      nameOf.set(atoms[name], name);
    }
    win = c.createInputOnly(root, -100, -100, 1, 1, MASK.PropertyChange);
    kinds.clipboard.atom = A.CLIPBOARD;
    kinds.primary.atom = A.PRIMARY;
    c.internAtoms(["_WLSERVER_CLIPBOARD", "_WLSERVER_PRIMARY"], (p) => {
      kinds.clipboard.prop = p._WLSERVER_CLIPBOARD;
      kinds.primary.prop = p._WLSERVER_PRIMARY;
    });
    xfixes = c.extensions.XFIXES || null;
    if (xfixes) {
      for (const k of Object.values(kinds)) {
        // XFixesSelectSelectionInput: owner changes, the owner's window destroyed, its client gone.
        c.ext("XFIXES", 2, (w) => w.u32(win).u32(k.atom).u32(1 | 2 | 4));
      }
    }
    // What the Wayland side has now is X's too.
    for (const kind of Object.keys(kinds)) {
      const sel = ws.currentSelection(kind);
      if (sel && !sel.x) {
        ws.xselectionChanged(kind, sel);
      }
    }
  };

  ws.xselectionWindow = () => win;

  ws.xselectionGone = () => {
    for (const [kind, k] of Object.entries(kinds)) {
      for (const job of [k.busy, ...k.queue]) {
        if (job) {
          sys1(SYS.close, job.fd);
        }
      }
      k.busy = null;
      k.queue = [];
      k.owned = null;
      const sel = ws.currentSelection(kind);
      if (sel && sel.x) {
        ws.setSelection(kind, null);	// (its owner was an X program: gone with X)
      }
    }
    for (const a of answers) {
      a.stop();
    }
    answers.clear();
    conn = null;
    win = 0;
  };

  // ---- events (xwm.js hands every event here first; true: it was ours)

  ws.xselectionEvent = (m, code) => {
    if (!conn) {
      return false;
    }
    if (xfixes && code === xfixes.event) {	// XFixesSelectionNotify
      ownerChanged(kindOf(m.u32(12)), m.u32(8));
      return true;
    }
    if (code === EV.SelectionNotify && m.u32(8) === win) {
      converted(kindOf(m.u32(12)), m.u32(16), m.u32(20));
      return true;
    }
    if (code === EV.PropertyNotify && m.u32(4) === win) {
      incrPiece(m.u32(8), m.u8(16));
      return true;
    }
    if (code === EV.SelectionRequest && m.u32(8) === win) {
      request(m.u32(4), m.u32(12), m.u32(16), m.u32(20), m.u32(24));
      return true;
    }
    if (code === EV.SelectionClear && m.u32(8) === win) {
      const kind = kindOf(m.u32(12));
      if (kind) {
        kinds[kind].owned = null;
      }
      return true;
    }
    if (code === EV.PropertyNotify && m.u8(16) === 1) {	// Deleted: maybe a requestor taking a piece (not used)
      return false;
    }
    return false;
  };

  // ---- X to Wayland

  const ownerChanged = (kind, owner) => {
    if (!kind || owner === win) {
      return;
    }
    const k = kinds[kind];
    if (owner === 0) {
      const sel = ws.currentSelection(kind);
      if (sel && sel.x) {
        ws.setSelection(kind, null);	// (the X program that had it let it go, or went)
      }
      return;
    }
    k.owned = null;
    // The owner's targets first.
    enqueue(kind, { target: A.TARGETS, fd: -1, owner });
  };

  const enqueue = (kind, job) => {
    const k = kinds[kind];
    k.queue.push(job);
    if (!k.busy) {
      next(kind);
    }
  };

  const next = (kind) => {
    const k = kinds[kind];
    k.busy = k.queue.shift() || null;
    if (k.busy) {
      k.busy.chunks = [];
      k.busy.length = 0;
      k.busy.incr = false;
      conn.convertSelection(win, k.atom, k.busy.target, k.prop || A._WLSERVER_SELECTION, 0);
    }
  };

  const done = (kind, data) => {
    const k = kinds[kind];
    const job = k.busy;
    if (!job) {
      return;
    }
    if (job.target === A.TARGETS) {
      offered(kind, data, job.owner);
    } else {
      if (data && job.latin1) {
        data = latin1ToUtf8(data);
      }
      if (data) {
        ws.writeAll(job.fd, data);
      }
      sys1(SYS.close, job.fd);
    }
    k.busy = null;
    next(kind);
  };

  // The owner's answer to a conversion: on our property (or none: it could not).
  const converted = (kind, target, property) => {
    if (!kind) {
      return;
    }
    const k = kinds[kind];
    const job = k.busy;
    if (!job || target !== job.target) {
      return;
    }
    if (!property) {
      done(kind, null);
      return;
    }
    conn.getProperty(win, property, (p) => {
      if (!p) {
        done(kind, null);
        return;
      }
      if (p.type === A.INCR) {
        job.incr = true;	// (the pieces come as the property is set again: incrPiece)
        return;
      }
      done(kind, job.target === A.TARGETS ? p : p.data);
    }, { del: true, max: MAX });
  };

  // INCR: the owner sets the property again for every piece (we delete it to say we have it); an empty piece
  // is the end.
  const incrPiece = (property, state) => {
    if (state !== 0) {
      return;	// (deleted: our own doing)
    }
    for (const kind of Object.keys(kinds)) {
      const k = kinds[kind];
      const job = k.busy;
      if (!job || !job.incr || property !== (k.prop || A._WLSERVER_SELECTION)) {
        continue;
      }
      conn.getProperty(win, property, (p) => {
        if (!p || !p.data.length) {
          const all = new Uint8Array(job.length);
          let at = 0;
          for (const c of job.chunks) {
            all.set(c, at);
            at += c.length;
          }
          done(kind, all);
          return;
        }
        if (job.length + p.data.length <= MAX) {
          job.chunks.push(p.data);
          job.length += p.data.length;
        }
      }, { del: true, max: MAX });
    }
  };

  // The owner's TARGETS: the Wayland selection becomes X's, with the types they map to.
  const offered = (kind, p, owner) => {
    const atoms = X.propertyWords(p && p.format === 32 ? p : null);
    if (!atoms.length) {
      return;
    }
    let left = atoms.length;
    const names = [];
    for (const a of atoms) {
      atomName(a, (name) => {
        names.push(name);
        if (--left > 0) {
          return;
        }
        const mimes = [];
        const targetOf = new Map();
        for (const n of names) {
          const m = mimeFor(n);
          if (m && !targetOf.has(m)) {
            mimes.push(m);
            targetOf.set(m, atomOf.get(n));
          }
        }
        // (UTF8_STRING is the one to ask for when there is text; an owner with only STRING, which is
        // Latin-1 (ICCCM 2.7.1), has its text made UTF-8 for the Wayland types)
        const text = names.includes("UTF8_STRING") ? "UTF8_STRING" : names.includes("STRING") ? "STRING" : null;
        if (text) {
          for (const m of ["text/plain;charset=utf-8", "UTF8_STRING", "text/plain"]) {
            if (!targetOf.has(m)) {
              mimes.push(m);
            }
            targetOf.set(m, atomOf.get(text));
          }
        }
        if (!mimes.length) {
          return;
        }
        const sel = {
          source: null, host: null, mimes,
          x: {
            owner,
            receive(mime, fd) {
              const target = targetOf.get(mime);
              if (!target || !conn || ws.currentSelection(kind) !== sel) {
                sys1(SYS.close, fd);
                return;
              }
              enqueue(kind, { target, fd, latin1: target === atomOf.get("STRING") && mime !== "STRING" });
            },
          },
        };
        if (ws.debug) {
          log("xselection: an X program's " + kind + ": " + mimes.join(", "));
        }
        ws.setSelection(kind, sel);
      });
    }
  };

  // ---- Wayland to X

  // From data.js: the Wayland selection changed. X's follows, unless it is an X program's.
  ws.xselectionChanged = (kind, sel) => {
    if (!conn || !win) {
      return;
    }
    const k = kinds[kind];
    if (sel && sel.x) {
      return;
    }
    if (!sel) {
      if (k.owned) {
        k.owned = null;
        conn.setSelectionOwner(0, k.atom, 0);
      }
      return;
    }
    const names = targetsFor(sel.mimes);
    intern(names, () => {
      if (ws.currentSelection(kind) !== sel || !conn) {
        return;
      }
      k.owned = { sel, targets: names };
      conn.setSelectionOwner(win, k.atom, 0);
    });
  };

  const answers = new Set();	// requests being answered (reading from a Wayland source)

  const notify = (requestor, selection, target, property, time) => {
    conn.sendEvent(requestor, false, MASK.NoEvent, (w) => {
      w.u8(EV.SelectionNotify).u8(0).u16(0).u32(time).u32(requestor).u32(selection).u32(target).u32(property);
    });
  };

  const request = (time, requestor, selection, target, property) => {
    const kind = kindOf(selection);
    const k = kind && kinds[kind];
    if (!property) {
      property = target;	// (an obsolete requestor: ICCCM 2.2)
    }
    if (!k || !k.owned || ws.currentSelection(kind) !== k.owned.sel) {
      notify(requestor, selection, target, 0, time);
      return;
    }
    const sel = k.owned.sel;
    if (target === A.TARGETS) {
      const list = [A.TARGETS, A.TIMESTAMP, ...k.owned.targets.map((n) => atomOf.get(n)).filter((a) => a)];
      conn.changeProperty(requestor, property, A.ATOM, 32, list);
      notify(requestor, selection, target, property, time);
      return;
    }
    if (target === A.TIMESTAMP) {
      conn.changeProperty(requestor, property, A.CARDINAL, 32, [time]);
      notify(requestor, selection, target, property, time);
      return;
    }
    atomName(target, (name) => {
      const mime = name && pickMime(sel.mimes, name);
      if (!mime || !conn) {
        if (conn) {
          notify(requestor, selection, target, 0, time);
        }
        return;
      }
      const type = name === "TEXT" ? A.UTF8_STRING : target;
      const reply = (data) => {
        if (!conn) {
          return;
        }
        if (name === "STRING" && mime !== "STRING") {
          data = utf8ToLatin1(data);	// (STRING is Latin-1: ICCCM 2.7.1)
        }
        // In pieces appended to the one property (each request well under the server's limit).
        if (!data.length) {
          conn.changeProperty(requestor, property, type, 8, data, 0);
        }
        for (let at = 0; at < data.length; at += PIECE) {
          conn.changeProperty(requestor, property, type, 8, data.subarray(at, at + PIECE), at === 0 ? 0 : 2);
        }
        notify(requestor, selection, target, property, time);
      };
      if (sel.host) {
        reply(sel.host);
        return;
      }
      if (!sel.source || sel.source.client.gone) {
        notify(requestor, selection, target, 0, time);
        return;
      }
      readSource(sel, mime, reply);
    });
  };

  // Read a type of a Wayland source through a pipe of ours; reply(bytes) at its end.
  const pair = ws.alloc(8);
  const buf = ws.alloc(65536);
  const readSource = (sel, mime, reply) => {
    if (sys2(SYS.pipe2, pair, O_CLOEXEC) < 0) {
      reply(new Uint8Array(0));
      return;
    }
    const v = ws.dv();
    const rfd = v.getInt32(pair, true), wfd = v.getInt32(pair + 4, true);
    sys3(SYS.fcntl, rfd, F_SETFL, O_NONBLOCK);
    ws.send(sel.source, "send", mime, wfd);
    ws.closeAfterSend(wfd);
    const job = { chunks: [], length: 0 };
    job.stop = () => {
      ws.unwatchFd(rfd);
      sys1(SYS.close, rfd);
      answers.delete(job);
    };
    answers.add(job);
    ws.watchFd(rfd, () => {
      for (;;) {
        const n = sys3(SYS.read, rfd, buf, 65536);
        if (n === -EINTR) {
          continue;
        }
        if (n === -EAGAIN) {
          return;
        }
        if (n <= 0) {
          break;
        }
        if (job.length + n <= MAX) {
          job.chunks.push(ws.u8().slice(buf, buf + n));
          job.length += n;
        }
      }
      job.stop();
      const all = new Uint8Array(job.length);
      let at = 0;
      for (const c of job.chunks) {
        all.set(c, at);
        at += c.length;
      }
      reply(all);
    });
  };
})
