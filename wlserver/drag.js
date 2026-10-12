// Drag and drop: wl_data_device.start_drag and the offers of a drag, and
// files from the host dropped on a window.
//
// A client starts a drag with a button down (wl_data_device.start_drag: a
// wl_data_source with its MIME types and the actions it allows, the surface it
// started on, and maybe an icon surface that follows the pointer). From then
// on until the button goes up, the pointer belongs to the drag: no client gets
// wl_pointer events; the client under the pointer gets a wl_data_offer of the
// source (data_offer, offer for each type, source_actions) and enter, motion
// and leave on its wl_data_device. It says what it would take
// (wl_data_offer.accept: a type or none; set_actions: the actions it can do
// and the one it prefers), which goes back to the source (target, action).
// When the button goes up over a client that accepted a type and with an
// action both sides can do, it gets drop (and the source dnd_drop_performed);
// it then receives the data the way a paste does (wl_data_offer.receive: a
// pipe's write end to the source's client) and says finish (the source
// dnd_finished). Otherwise the target gets leave and the source cancelled.
// A drag whose source is null is within one client (it knows the data): only
// that client's surfaces get offers, and the offer has no types.
//
// The icon surface (role "dnd_icon") is a surface of the table, kind 5, at the
// pointer plus its offset, above everything; the page draws it like any other.
//
// Files from the host (data.js: files): once they are in ~/Downloads, the
// window they were dropped on is offered them as a drop of our own, with
// text/uri-list (file:// URIs) and text/plain (the paths): enter at the drop's
// place, a moment for the client to accept, drop, and its receive answered by
// us.
(function (ws) {
  "use strict";

  const { send, log, sys1, SYS, table, H } = ws;
  const ACTION_NONE = 0, ACTION_COPY = 1, ACTION_MOVE = 2, ACTION_ASK = 4;
  const HOST_WAIT_MS = 400, HOST_GIVE_UP_MS = 10000;

  let drag = null;
  // { source: wl_data_source or null, host: { "mime": bytes } or null, client (the one that started it), icon,
  //   iconX, iconY, focus (surface), offers: [wl_data_offer], accepted: mime or null, sourceActions, action,
  //   dropped, started (ms), droppedAt (ms) }

  const now = () => (performance.now() | 0) >>> 0;
  const devicesOf = (client) => (client.devices ? client.devices.clipboard : new Set());
  const serverId = (client) => {
    client.nextServerId = client.nextServerId ? client.nextServerId + 1 : 0xff000000;
    return client.nextServerId;
  };
  const alive = (o) => o && o.client && !o.client.gone && o.client.objects.get(o.id) === o;

  ws.dragging = () => !!drag && !drag.dropped;

  // ---- where the icon is

  ws.dragIconPos = (surface) => {
    const p = ws.pointerState();
    if (!drag || drag.icon !== surface) {
      return [p.x, p.y];
    }
    return [p.x + drag.iconX, p.y + drag.iconY];
  };

  // From compositor.js: a commit to the icon (its offset moves it, as attach's x and y or wl_surface.offset do).
  ws.dragIconCommit = (surface, st) => {
    if (drag && drag.icon === surface && st.offset) {
      drag.iconX += st.offset[0];
      drag.iconY += st.offset[1];
    }
  };

  // ---- the action both sides can do

  const choose = (offer) => {
    const sa = drag.source ? (drag.source.version >= 3 ? drag.source.actions || 0 : ACTION_COPY) : ACTION_COPY;
    const oa = offer.version >= 3 ? offer.actions || 0 : ACTION_COPY;
    const both = sa & oa;
    if (offer.preferred & both) {
      return offer.preferred;
    }
    for (const a of [ACTION_COPY, ACTION_MOVE, ACTION_ASK]) {
      if (both & a) {
        return a;
      }
    }
    return ACTION_NONE;
  };

  const actionChanged = (offer) => {
    const a = choose(offer);
    if (a === drag.action && offer.toldAction === a) {
      return;
    }
    drag.action = a;
    offer.toldAction = a;
    send(offer, "action", a);
    if (alive(drag.source)) {
      send(drag.source, "action", a);
    }
  };

  // ---- focus: which surface the drag is over

  const leave = () => {
    if (!drag || !drag.focus) {
      return;
    }
    const client = drag.focus.object.client;
    if (!client.gone) {
      for (const device of devicesOf(client)) {
        send(device, "leave");
      }
    }
    for (const offer of drag.offers) {
      offer.dnd = null;	// (dead: the client destroys it)
    }
    drag.offers = [];
    drag.focus = null;
    drag.accepted = null;
    if (alive(drag.source)) {
      send(drag.source, "target", null);
    }
  };

  const enter = (surface, x, y) => {
    leave();
    if (!surface) {
      return;
    }
    const client = surface.object.client;
    // (A drag with no source stays within its client.)
    if (!drag.source && !drag.host && client !== drag.client) {
      return;
    }
    drag.focus = surface;
    const serial = ws.nextSerial();
    const mimes = drag.source ? drag.source.mimes : drag.host ? Object.keys(drag.host) : [];
    for (const device of devicesOf(client)) {
      let offer = null;
      if (drag.source || drag.host) {
        offer = ws.create(client, serverId(client), "wl_data_offer", device.version);
        offer.dnd = drag;
        offer.actions = 0;
        offer.preferred = 0;
        drag.offers.push(offer);
        send(device, "data_offer", offer);
        for (const mime of mimes) {
          send(offer, "offer", mime);
        }
        send(offer, "source_actions", drag.source ? (drag.source.version >= 3 ? drag.source.actions || 0 : ACTION_COPY)
                                                  : ACTION_COPY);
      }
      send(device, "enter", serial, surface.object, x, y, offer);
    }
  };

  const motion = (surface, x, y, time) => {
    if (surface !== drag.focus) {
      enter(surface, x, y);
      return;
    }
    if (surface) {
      for (const device of devicesOf(surface.object.client)) {
        send(device, "motion", time, x, y);
      }
    }
  };

  // From seat.js: the pointer moved while a drag holds it.
  ws.dragMotion = (time) => {
    const p = ws.pointerState();
    const target = ws.surfaceAt(p.x, p.y);
    let x = 0, y = 0;
    if (target) {
      const pos = ws.screenPos(target);
      x = p.x - pos[0];
      y = p.y - pos[1];
    }
    motion(target, x, y, time);
    if (drag && drag.icon && drag.icon.slot >= 0) {
      drag.icon.dirty = true;
    }
  };

  // ---- the end

  const finish = (why) => {
    if (!drag) {
      return;
    }
    const d = drag;
    drag = null;
    table.drag(0);
    if (d.icon) {
      ws.unmapSurface(d.icon);
    }
    log("drag: over (" + why + ")");
    ws.seatChanged();
  };

  const cancel = (why) => {
    if (!drag) {
      return;
    }
    leave();
    if (alive(drag.source)) {
      send(drag.source, "cancelled");
    }
    finish(why);
  };

  // From seat.js: the last button went up. A drop, or nothing.
  ws.dragDrop = () => {
    if (!drag || drag.dropped) {
      return;
    }
    const offer = drag.offers[0];
    const v3 = offer && offer.version >= 3;
    if (drag.focus && offer && drag.accepted !== null && (!v3 || drag.action !== ACTION_NONE)) {
      for (const device of devicesOf(drag.focus.object.client)) {
        send(device, "drop");
      }
      if (alive(drag.source)) {
        send(drag.source, "dnd_drop_performed");
      }
      drag.dropped = true;
      drag.droppedAt = performance.now();
      table.drag(0);
      if (drag.icon) {
        ws.unmapSurface(drag.icon);
        drag.icon = null;
      }
      ws.seatChanged();
      if (!v3) {
        finish("dropped (version 1 or 2: no finish to wait for)");
      }
      return;
    }
    if (!drag.source && !drag.host && drag.focus) {
      // (Within one client and no offer: it is told by drop all the same, and knows what was dragged.)
      for (const device of devicesOf(drag.focus.object.client)) {
        send(device, "drop");
      }
      finish("dropped within the client");
      return;
    }
    cancel("let go where nothing took it");
  };

  // ---- the requests

  const originalStartDrag = H.wl_data_device.start_drag;
  H.wl_data_device.start_drag = (device, source, origin, icon, serial) => {
    const p = ws.pointerState();
    if (drag || !p.buttons) {
      // (No button down, or a drag going on already: there is nothing to drag with.)
      log("drag: refused (" + (drag ? "another drag is going on" : "no button is down") + ")");
      if (source) {
        send(source, "cancelled");
      }
      return;
    }
    if (icon && icon.surface.role && icon.surface.role !== "dnd_icon") {
      ws.error(device.client, device, 0, "start_drag: the icon surface has another role");
      return;
    }
    drag = { source, host: null, client: device.client, icon: icon ? icon.surface : null, iconX: 0, iconY: 0,
             focus: null, offers: [], accepted: null, action: ACTION_NONE, dropped: false, started: performance.now() };
    if (drag.icon) {
      drag.icon.role = "dnd_icon";
      if (drag.icon.current.buffer) {
        ws.mapSurface(drag.icon);	// (it was drawn before it was an icon: shown now)
      }
    }
    if (source) {
      source.dragging = true;
    }
    table.drag(1);
    ws.leavePointer();
    ws.dragMotion(now());
    log("drag: started by client " + device.client.fd + " with " + (source ? source.mimes.join(", ") : "no source"));
  };
  void originalStartDrag;

  // A source's actions (version 3).
  const originalSetActions = H.wl_data_source.set_actions;
  H.wl_data_source.set_actions = (source, actions) => {
    source.actions = actions;
    if (drag && drag.source === source && drag.offers[0]) {
      actionChanged(drag.offers[0]);
    }
    if (originalSetActions) {
      originalSetActions(source, actions);
    }
  };

  // The offers: a drag's, or the selection's (data.js).
  const selectionOffer = H.wl_data_offer;
  const dndOffer = {
    accept(offer, serial, mime) {
      const d = offer.dnd;
      if (!d || d !== drag) {
        return;
      }
      d.accepted = mime;
      if (alive(d.source)) {
        send(d.source, "target", mime);
      }
    },
    set_actions(offer, actions, preferred) {
      if (!offer.dnd || offer.dnd !== drag) {
        return;
      }
      offer.actions = actions;
      offer.preferred = preferred;
      actionChanged(offer);
    },
    receive(offer, mime, fd) {
      const d = offer.dnd;
      if (!d) {
        sys1(SYS.close, fd);
        return;
      }
      if (d.host) {
        ws.writeAll(fd, d.host[mime] || new Uint8Array(0));
        sys1(SYS.close, fd);
        return;
      }
      if (!alive(d.source)) {
        sys1(SYS.close, fd);
        return;
      }
      send(d.source, "send", mime, fd);
      ws.closeAfterSend(fd);
    },
    finish(offer) {
      const d = offer.dnd;
      if (!d || d !== drag || !d.dropped) {
        return;
      }
      if (alive(d.source)) {
        send(d.source, "dnd_finished");
      }
      finish("finished");
    },
    destroy(offer) {
      const d = offer.dnd;
      if (d && d === drag && d.dropped) {
        // (Destroyed after the drop without finish: version 3 says that is the end too.)
        if (alive(d.source)) {
          send(d.source, "dnd_finished");
        }
        finish("the offer was destroyed after the drop");
      }
    },
  };
  H.wl_data_offer = {};
  for (const name of new Set([...Object.keys(selectionOffer), ...Object.keys(dndOffer)])) {
    H.wl_data_offer[name] = (offer, ...args) => {
      const h = offer.dnd ? dndOffer[name] : selectionOffer[name];
      if (h) {
        h(offer, ...args);
      }
    };
  }

  // Things going away in the middle of a drag.
  ws.dragObjectGone = (object) => {
    if (!drag) {
      return;
    }
    if (object === drag.source) {
      if (drag.dropped) {
        finish("its source is gone after the drop");	// (Kate does so after moving text within itself)
      } else {
        cancel("its source is gone");
      }
    } else if (object.surface && object.surface === drag.focus) {
      drag.focus = null;
      drag.offers = [];
    }
  };
  ws.dragSurfaceGone = (surface) => {
    if (!drag) {
      return;
    }
    if (surface === drag.icon) {
      drag.icon = null;
    }
    if (surface === drag.focus) {
      drag.focus = null;
      drag.offers = [];
      if (drag.dropped) {
        finish("the window it was dropped on is gone");
      }
    }
  };
  ws.dragClientGone = (client) => {
    if (drag && (drag.client === client || (drag.focus && drag.focus.object.client === client))) {
      if (drag.client === client && !drag.host) {
        cancel("the client that started it is gone");
      } else {
        drag.focus = null;
        drag.offers = [];
        if (drag.dropped) {
          finish("the client it was dropped on is gone");
        }
      }
    }
  };

  // ---- files from the host, dropped on a window (data.js calls this once they are in ~/Downloads)

  const uri = (path) => "file://" + path.split("/").map((part) => encodeURIComponent(part)).join("/");

  ws.dropFiles = (paths, x, y) => {
    if (drag) {
      log("drag: files dropped on a window during a drag: not offered");
      return;
    }
    const target = ws.surfaceAt(x, y);
    if (!target) {
      return;
    }
    const enc = new TextEncoder();
    const host = {
      "text/uri-list": enc.encode(paths.map(uri).join("\r\n") + "\r\n"),
      "text/plain;charset=utf-8": enc.encode(paths.join("\n")),
      "text/plain": enc.encode(paths.join("\n")),
    };
    drag = { source: null, host, client: null, icon: null, iconX: 0, iconY: 0, focus: null, offers: [], accepted: null,
             action: ACTION_NONE, dropped: false, started: performance.now(), hostTarget: target };
    const pos = ws.screenPos(target);
    enter(target, x - pos[0], y - pos[1]);
    log("drag: " + paths.length + " file(s) from the host offered to the window under " + x + "," + y);
  };

  // Every turn (server.js): a drop of host files goes on when the client has had a moment to accept.
  ws.dragTurn = () => {
    if (!drag || !drag.host) {
      return;
    }
    const t = performance.now();
    if (!drag.dropped) {
      if (!drag.focus) {
        finish("the window is gone");
      } else if (drag.accepted !== null || t - drag.started > HOST_WAIT_MS) {
        if (drag.accepted === null && drag.offers[0]) {
          drag.accepted = "text/uri-list";	// (it did not say: dropped all the same, the client may take it)
        }
        if (drag.action === ACTION_NONE) {
          drag.action = ACTION_COPY;
        }
        ws.dragDrop();
      }
    } else if (t - drag.droppedAt > HOST_GIVE_UP_MS) {
      finish("no finish from the window");
    }
  };
})
