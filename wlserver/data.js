// The clipboard: wl_data_device_manager and its family, and the primary
// selection (zwp_primary_selection_device_manager_v1: what is selected,
// pasted with the middle button), which works the same way; and the bridge
// between the guest's clipboard and the host browser's.
//
// A selection is a source: a client's (wl_data_source, with the MIME types it
// offers) or, for text that came from the host, ours. The client with the
// keyboard is given an offer of it (a wl_data_offer made by us) whenever the
// selection changes and whenever it gets the keyboard. A client that wants
// the data sends a descriptor (the write end of a pipe) with
// wl_data_offer.receive; we pass it on to the source's client
// (wl_data_source.send) and let go of our copy, or, for host text, write the
// text into it ourselves. Drag and drop is not done: a drag is cancelled.
//
// The host bridge (README.md: the clipboard): when a client puts text on the
// clipboard, we ask it for the text at once (a pipe of our own) and put it in
// the table for the page, which hands it to navigator.clipboard.writeText.
// The other way, the page puts the host's clipboard text in the table when
// the person presses Ctrl-V (or Ctrl-Shift-V, or Cmd-V) and holds that key
// back until we have made the text the selection; the key then reaches the
// client after the offer.
(function (ws) {
  "use strict";

  const { send, log, sys1, sys2, sys3, SYS, table, H, global } = ws;
  const O_NONBLOCK = 0o4000, O_CLOEXEC = 0o2000000, F_SETFL = 4;
  const EAGAIN = 11, EINTR = 4;
  const TEXT_TYPES = ["text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"];
  const MAX_TEXT = 4 << 20;

  const kinds = {
    clipboard: { manager: "wl_data_device_manager", device: "wl_data_device", source: "wl_data_source",
                 offer: "wl_data_offer", current: null },
    primary: { manager: "zwp_primary_selection_device_manager_v1", device: "zwp_primary_selection_device_v1",
               source: "zwp_primary_selection_source_v1", offer: "zwp_primary_selection_offer_v1", current: null },
  };

  const devicesOf = (client, kind) => {
    const all = client.devices || (client.devices = { clipboard: new Set(), primary: new Set() });
    return all[kind];
  };

  const serverId = (client) => {
    client.nextServerId = client.nextServerId ? client.nextServerId + 1 : 0xff000000;
    return client.nextServerId;
  };

  // Tell a client's devices of `kind` what the selection is now.
  const offerTo = (client, kind) => {
    if (!client || client.gone) {
      return;
    }
    const k = kinds[kind];
    const sel = k.current;
    for (const device of devicesOf(client, kind)) {
      if (!sel) {
        send(device, "selection", null);
        continue;
      }
      const offer = ws.create(client, serverId(client), k.offer, device.version);
      offer.selection = sel;
      offer.kind = kind;
      send(device, "data_offer", offer);
      for (const mime of sel.mimes) {
        send(offer, "offer", mime);
      }
      send(device, "selection", offer);
    }
  };

  // From seat.js: a client is getting the keyboard; it is told the selections first.
  ws.selectionsTo = (client) => {
    offerTo(client, "clipboard");
    offerTo(client, "primary");
  };

  const setSelection = (kind, sel) => {
    const k = kinds[kind];
    const old = k.current;
    if (old && old.source && (!sel || old.source !== sel.source) && !old.source.client.gone) {
      send(old.source, "cancelled");
    }
    k.current = sel;
    offerTo(ws.keyboardClient(), kind);
    if (kind === "clipboard" && sel && (sel.source || sel.x)) {
      table.guestSelection();
      readForHost(sel);
    }
    if (ws.xselectionChanged) {
      ws.xselectionChanged(kind, sel);	// (X's selection follows: xselection.js)
    }
  };
  // (A selection is { source, mimes, host, x }: a Wayland client's source, or text from the host, or an X
  // program's, which xselection.js reads with x.receive(mime, fd).)
  ws.setSelection = setSelection;
  ws.currentSelection = (kind) => kinds[kind].current;

  // ---- the requests, the same for both kinds

  const sourceHandlers = (kind) => ({
    offer(source, mime) {
      if (mime && !source.mimes.includes(mime)) {
        source.mimes.push(mime);
      }
    },
    destroy() {},
    set_actions() {},
  });

  const deviceHandlers = (kind) => ({
    set_selection(device, source) {
      setSelection(kind, source ? { source, mimes: source.mimes.slice(), host: null } : null);
    },
    start_drag(device, source) {
      // Drag and drop is not done here: the drag ends at once.
      if (source) {
        send(source, "cancelled");
      }
    },
    release() {},
    destroy() {},
  });

  const offerHandlers = (kind) => ({
    accept() {},
    finish() {},
    set_actions() {},
    destroy() {},
    receive(offer, mime, fd) {
      const sel = offer.selection;
      if (sel !== kinds[kind].current || !sel.mimes.includes(mime)) {
        sys1(SYS.close, fd);
        return;
      }
      if (sel.host) {
        ws.writeAll(fd, sel.host);
        sys1(SYS.close, fd);
        return;
      }
      if (sel.x) {
        sel.x.receive(mime, fd);	// (an X program's: xselection.js writes it and closes the descriptor)
        return;
      }
      if (sel.source.client.gone) {
        sys1(SYS.close, fd);
        return;
      }
      send(sel.source, "send", mime, fd);
      ws.closeAfterSend(fd);
    },
  });

  const makeSource = (client, id, iface, version, kind) => {
    const source = ws.create(client, id, iface, version);
    source.mimes = [];
    source.onDestroy = () => {
      if (ws.dragObjectGone) {
        ws.dragObjectGone(source);
      }
      const k = kinds[kind];
      if (k.current && k.current.source === source) {
        k.current = null;
        offerTo(ws.keyboardClient(), kind);
      }
    };
    return source;
  };

  const makeDevice = (client, id, iface, version, kind) => {
    const device = ws.create(client, id, iface, version);
    devicesOf(client, kind).add(device);
    device.onDestroy = () => devicesOf(client, kind).delete(device);
    if (ws.keyboardClient() === client) {
      offerTo(client, kind);
    }
    return device;
  };

  H.wl_data_device_manager = {
    create_data_source(manager, id) {
      makeSource(manager.client, id, "wl_data_source", manager.version, "clipboard");
    },
    get_data_device(manager, id) {
      makeDevice(manager.client, id, "wl_data_device", manager.version, "clipboard");
    },
  };
  H.wl_data_source = sourceHandlers("clipboard");
  H.wl_data_device = deviceHandlers("clipboard");
  H.wl_data_offer = offerHandlers("clipboard");

  H.zwp_primary_selection_device_manager_v1 = {
    create_source(manager, id) {
      makeSource(manager.client, id, "zwp_primary_selection_source_v1", manager.version, "primary");
    },
    get_device(manager, id) {
      makeDevice(manager.client, id, "zwp_primary_selection_device_v1", manager.version, "primary");
    },
    destroy() {},
  };
  H.zwp_primary_selection_source_v1 = sourceHandlers("primary");
  H.zwp_primary_selection_device_v1 = deviceHandlers("primary");
  H.zwp_primary_selection_offer_v1 = offerHandlers("primary");

  global("wl_data_device_manager", 3, () => {});
  global("zwp_primary_selection_device_manager_v1", 1, () => {});

  // ---- the host bridge: guest to host

  const pair = ws.alloc(8);
  const chunk = ws.alloc(32768);
  let reading = null;	// { fd, chunks, length, sel }

  const stopReading = () => {
    if (reading) {
      ws.unwatchFd(reading.fd);
      sys1(SYS.close, reading.fd);
      reading = null;
    }
  };

  const readForHost = (sel) => {
    stopReading();
    const mime = TEXT_TYPES.find((t) => sel.mimes.includes(t));
    if (!mime || (sel.source && sel.source.client.gone)) {
      return;
    }
    const r = sys2(SYS.pipe2, pair, O_CLOEXEC);
    if (r < 0) {
      log("data: pipe2: error " + -r);
      return;
    }
    const v = ws.dv();
    const rfd = v.getInt32(pair, true), wfd = v.getInt32(pair + 4, true);
    sys3(SYS.fcntl, rfd, F_SETFL, O_NONBLOCK);	// (ours only; the client's end blocks as it expects)
    if (sel.x) {
      sel.x.receive(mime, wfd);
    } else {
      send(sel.source, "send", mime, wfd);
      ws.closeAfterSend(wfd);
    }
    reading = { fd: rfd, chunks: [], length: 0, sel };
    const me = reading;
    ws.watchFd(rfd, () => drain(me));
  };

  const drain = (me) => {
    if (reading !== me) {
      return;
    }
    const buf = chunk;
    for (;;) {
      const n = sys3(SYS.read, me.fd, buf, 32768);
      if (n === -EINTR) {
        continue;
      }
      if (n === -EAGAIN) {
        return;
      }
      if (n <= 0) {
        break;	// the end (or an error: what came is kept)
      }
      if (me.length + n <= MAX_TEXT) {
        me.chunks.push(ws.u8().slice(buf, buf + n));
        me.length += n;
      }
    }
    const text = new Uint8Array(me.length);
    let at = 0;
    for (const c of me.chunks) {
      text.set(c, at);
      at += c.length;
    }
    stopReading();
    if (kinds.clipboard.current === me.sel) {
      table.guestClipboard(text);
      if (ws.debug) {
        log("data: " + text.length + " bytes of the guest's clipboard for the host");
      }
    }
  };

  // ---- files from the host (dropped on the page): into ~/Downloads
  //
  // The page hands a file over a piece at a time (table.js: transfer); the first piece names it, the last
  // closes it, and the last piece of the last file of a drop says where on the screen the drop was (if on a
  // window, that window is offered the files: drag.js). The name is the file's own, as a name only (no
  // directories, no leading dots); a name that is taken gets a number. Files are created, never overwritten
  // (O_EXCL), and at most MAX_FILE bytes each.

  const O_WRONLY = 1, O_CREAT = 0o100, O_EXCL = 0o200, O_DIRECTORY = 0o200000, AT_FDCWD = -100;
  const EEXIST = 17, EFBIG = 27, EINVAL = 22;
  const MAX_FILE = 512 << 20;
  const home = (ws.envp.find((e) => e.startsWith("HOME=")) || "HOME=/root").slice(5) || "/root";
  const downloads = home + "/Downloads";
  let incoming = null;	// { fd, path, written, total }
  let dropped = [];	// paths of this drop's files so far

  const safeName = (name) => {
    let n = String(name || "").split(/[\/\\]/).pop().replace(/[\x00-\x1f\x7f]/g, "").replace(/^[.\s]+/, "").trim();
    const enc = new TextEncoder();
    while (enc.encode(n).length > 200) {
      n = n.slice(0, -1);
    }
    return n || "dropped";
  };

  const create = (name) => {
    sys3(SYS.mkdirat, AT_FDCWD, ws.path(downloads, ws.P0), 0o755);
    const dot = name.lastIndexOf(".");
    const stem = dot > 0 ? name.slice(0, dot) : name, ext = dot > 0 ? name.slice(dot) : "";
    for (let i = 0; i < 1000; i++) {
      const path = downloads + "/" + (i ? stem + " (" + i + ")" + ext : name);
      const fd = ws.sys4(SYS.openat, AT_FDCWD, ws.path(path, ws.P0), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o644);
      if (fd >= 0) {
        return { fd, path };
      }
      if (fd !== -EEXIST) {
        return { fd, path };
      }
    }
    return { fd: -EEXIST, path: "" };
  };

  const transferTurn = () => {
    const piece = table.transfer();
    if (!piece) {
      return;
    }
    let status = 0;
    if (piece.flags & 1) {
      if (incoming) {
        sys1(SYS.close, incoming.fd);	// (a file the page never finished)
      }
      incoming = null;
      if (piece.total > MAX_FILE) {
        status = EFBIG;
      } else {
        const made = create(safeName(piece.name));
        if (made.fd < 0) {
          status = -made.fd;
        } else {
          incoming = { fd: made.fd, path: made.path, written: 0, total: piece.total };
        }
      }
    }
    if (!status && !incoming) {
      status = EINVAL;	// (a piece of a file whose first piece did not come or failed)
    }
    if (!status) {
      if (incoming.written + piece.length > incoming.total) {
        status = EFBIG;
      } else {
        // (Straight from the table's block: it is in this process's memory, as everything is.)
        let done = 0;
        while (done < piece.length) {
          const n = sys3(SYS.write, incoming.fd, piece.address + done, piece.length - done);
          if (n === -EINTR) {
            continue;
          }
          if (n <= 0) {
            status = n < 0 ? -n : 28;
            break;
          }
          done += n;
        }
        incoming.written += done;
      }
    }
    const path = incoming ? incoming.path : "";
    if (status && incoming) {
      sys1(SYS.close, incoming.fd);
      ws.sys3(SYS.unlinkat, AT_FDCWD, ws.path(incoming.path, ws.P0), 0);
      incoming = null;
    } else if (piece.flags & 2) {
      sys1(SYS.close, incoming.fd);
      log("data: " + incoming.written + " bytes from the host in " + incoming.path);
      dropped.push(incoming.path);
      incoming = null;
    }
    if (piece.flags & 4) {
      if (dropped.length && piece.x >= 0 && piece.y >= 0 && ws.dropFiles) {
        ws.dropFiles(dropped, piece.x, piece.y);
      }
      dropped = [];
    }
    table.transferDone(piece.serial, status, path);
  };

  // ---- the host bridge: host to guest (every turn)

  ws.dataTurn = () => {
    transferTurn();
    const got = table.hostClipboard();
    if (!got) {
      return;
    }
    setSelection("clipboard", { source: null, mimes: TEXT_TYPES.slice(), host: got.bytes });
    table.hostClipboardTaken(got.serial);
    if (ws.debug) {
      log("data: " + got.bytes.length + " bytes from the host's clipboard are the selection now");
    }
  };
})
