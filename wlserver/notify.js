// Notifications and tray icons, the server's half: the services on the
// session bus (dbus.js) that programs send them to, and the page told on the
// rv-plasma channel, which draws them (PLASMA-PROTOCOL.md, 3 and 4).
//
// org.freedesktop.Notifications (the Desktop Notifications Specification,
// version 1.2, as Plasma's notification server answers it): Notify,
// CloseNotification, GetCapabilities, GetServerInformation; the signals
// NotificationClosed, ActionInvoked and ActivationToken. Konsole and every
// KDE program send theirs through KNotification, which uses this; so do
// notify-send, GTK programs and Firefox.
//
// org.kde.StatusNotifierWatcher (KDE's StatusNotifierItem specification, which
// freedesktop.org has as a draft): programs register their tray items; the
// watcher is also the host here (IsStatusNotifierHostRegistered is true, so
// that programs use it rather than an X tray). Each item's properties are
// read from it (org.kde.StatusNotifierItem: Id, Title, Status, Category,
// IconName, IconPixmap, IconThemePath, ToolTip) and read again when it says
// they changed; an item goes when its program leaves the bus.
(function (ws) {
  "use strict";

  const { log, dbus } = ws;
  if (!dbus || !dbus.connected) {
    return;
  }
  const { V } = dbus;
  const channel = ws.channel("rv-plasma");
  const N = "org.freedesktop.Notifications";
  const PATH = "/org/freedesktop/Notifications";

  // ---- notifications

  let lastId = 0;
  const live = new Map();	// id -> { sender, actions }

  const image = (v) => {
    // (iiibiiay): width, height, rowstride, has alpha, bits per sample, channels, the pixels
    if (!Array.isArray(v) || v.length < 7 || !(v[6] instanceof Uint8Array)) {
      return null;
    }
    const [width, height, rowstride, hasAlpha, bitsPerSample, channels, data] = v;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || data.length < rowstride * (height - 1) + width * channels) {
      return null;
    }
    return { width, height, rowstride, hasAlpha, bitsPerSample, channels, data };
  };

  const notify = ([appName, replaces, appIcon, summary, body, actions, hints, timeout], m) => {
    const id = replaces && live.has(replaces) ? replaces : ++lastId;
    const pairs = [];
    for (let i = 0; i + 1 < actions.length; i += 2) {
      pairs.push([actions[i], actions[i + 1]]);
    }
    const h = hints || {};
    const path = (x) => (typeof x === "string" && x ? x.replace(/^file:\/\//, "") : null);
    const imagePath = path(h["image-path"]) || path(h.image_path) || (appIcon && (appIcon.startsWith("/") || appIcon.startsWith("file://")) ? path(appIcon) : null);
    live.set(id, { sender: m.sender, actions: pairs, resident: h.resident === true });
    const n = {
      type: "notify", id, appName, appIcon: imagePath && imagePath === path(appIcon) ? "" : appIcon, summary, body, actions: pairs,
      urgency: typeof h.urgency === "number" ? h.urgency : 1, timeout,
      desktopEntry: typeof h["desktop-entry"] === "string" ? h["desktop-entry"] : "",
      category: typeof h.category === "string" ? h.category : "",
      resident: h.resident === true, transient: h.transient === true,
      image: image(h["image-data"]) || image(h.image_data) || image(h.icon_data),
      imagePath, replaces: id === replaces, time: Date.now(),
    };
    channel.postMessage(n);
    log("notify: " + id + " from " + (appName || m.sender) + ": " + JSON.stringify(summary) + (body ? " " + JSON.stringify(body.slice(0, 80)) : "")
        + (pairs.length ? " [" + pairs.map((p) => p[1]).join(", ") + "]" : ""));
    return id;
  };

  const closed = (id, reason) => {
    if (!live.has(id)) {
      return;
    }
    live.delete(id);
    dbus.emit(PATH, N, "NotificationClosed", "uu", [id, reason]);
  };

  dbus.export(PATH, N, {
    methods: {
      GetCapabilities: { out: "as", outNames: ["capabilities"], fn: () => ["body", "body-markup", "body-hyperlinks", "body-images",
        "icon-static", "actions", "persistence", "action-icons", "x-kde-display-appname", "x-kde-origin-name"] },
      Notify: { in: "susssasa{sv}i", out: "u",
                inNames: ["app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "timeout"], outNames: ["id"],
                fn: notify },
      CloseNotification: { in: "u", inNames: ["id"], fn: ([id]) => {
        if (live.has(id)) {
          channel.postMessage({ type: "notify-close", id });
          closed(id, 3);
        }
      } },
      GetServerInformation: { out: "ssss", outNames: ["name", "vendor", "version", "spec_version"],
                              fn: () => ["Plasma", "KDE", "6.3.6", "1.2"] },
    },
    signals: {
      NotificationClosed: { type: "uu", names: ["id", "reason"] },
      ActionInvoked: { type: "us", names: ["id", "action_key"] },
      ActivationToken: { type: "us", names: ["id", "activation_token"] },
    },
  });
  // (plasmashell's own notifications applet is the server while plasmashell is the desktop's shell)
  if (!ws.nativeShell) {
    dbus.own(N);
  }

  // ---- tray items

  const W = "org.kde.StatusNotifierWatcher";
  const ITEM = "org.kde.StatusNotifierItem";
  const items = new Map();	// key (service + path) -> { service, path, props }

  const sendTray = () => {
    channel.postMessage({ type: "tray", items: [...items.entries()].map(([key, it]) => {
      const p = it.props || {};
      const pix = Array.isArray(p.IconPixmap) && p.IconPixmap.length
        ? p.IconPixmap.slice().sort((a, b) => b[0] - a[0])[0] : null;	// (the biggest; ARGB32, network byte order)
      return {
        key, id: p.Id || "", title: p.Title || "", status: p.Status || "Active", category: p.Category || "",
        iconName: (p.Status === "NeedsAttention" && p.AttentionIconName) || p.IconName || "", iconThemePath: p.IconThemePath || "",
        iconPixmap: pix ? { width: pix[0], height: pix[1], data: pix[2] } : null,
        tooltip: Array.isArray(p.ToolTip) ? [p.ToolTip[2], p.ToolTip[3]].filter(Boolean).join("\n") : "",
      };
    }) });
  };

  const refresh = (key) => {
    const it = items.get(key);
    if (!it) {
      return;
    }
    dbus.call(it.service, it.path, "org.freedesktop.DBus.Properties", "GetAll", "s", [ITEM]).then(([props]) => {
      if (items.get(key) === it) {
        it.props = props;
        sendTray();
      }
    }, (error) => log("notify: tray item " + key + ": " + error.message));
  };

  const register = (service, sender) => {
    // The argument is a bus name (the item at /StatusNotifierItem) or an object path (on the caller's connection).
    let name = service, path = "/StatusNotifierItem";
    if (service.startsWith("/")) {
      name = sender;
      path = service;
    }
    const key = name + path;
    if (!items.has(key)) {
      items.set(key, { service: name, path, owner: sender, props: null });
      dbus.emit("/StatusNotifierWatcher", W, "StatusNotifierItemRegistered", "s", [key]);
      log("notify: tray item " + key);
    }
    refresh(key);
  };

  dbus.export("/StatusNotifierWatcher", W, {
    methods: {
      RegisterStatusNotifierItem: { in: "s", inNames: ["service"], fn: ([service], m) => register(service, m.sender) },
      RegisterStatusNotifierHost: { in: "s", inNames: ["service"], fn: () => {} },
    },
    properties: {
      RegisteredStatusNotifierItems: { type: "as", get: () => [...items.keys()] },
      IsStatusNotifierHostRegistered: { type: "b", get: () => true },
      ProtocolVersion: { type: "i", get: () => 0 },
    },
    signals: {
      StatusNotifierItemRegistered: "s", StatusNotifierItemUnregistered: "s",
      StatusNotifierHostRegistered: "", StatusNotifierHostUnregistered: "",
    },
  });
  dbus.own(W).then((ok) => {
    if (ok) {
      dbus.emit("/StatusNotifierWatcher", W, "StatusNotifierHostRegistered", "", []);
    }
  });

  // An item says something changed: read it again.
  for (const member of ["NewIcon", "NewTitle", "NewStatus", "NewToolTip", "NewAttentionIcon", "NewIconThemePath"]) {
    dbus.match({ type: "signal", interface: ITEM, member }, (m) => {
      for (const [key, it] of items) {
        if ((it.owner === m.sender || it.service === m.sender) && it.path === m.path) {
          refresh(key);
        }
      }
    });
  }
  // A program left the bus: its items and notifications' actions go.
  dbus.match({ type: "signal", sender: "org.freedesktop.DBus", interface: "org.freedesktop.DBus", member: "NameOwnerChanged" }, (m) => {
    const [name, , owner] = m.body;
    if (owner) {
      return;
    }
    let changed = false;
    for (const [key, it] of [...items]) {
      if (it.service === name || it.owner === name) {
        items.delete(key);
        dbus.emit("/StatusNotifierWatcher", W, "StatusNotifierItemUnregistered", "s", [key]);
        changed = true;
      }
    }
    if (changed) {
      sendTray();
    }
  });

  // ---- what the page says

  channel.onmessage = (event) => {
    const q = event.data;
    if (!q || typeof q !== "object") {
      return;
    }
    if (q.type === "notify-closed" && live.has(q.id)) {
      closed(q.id, [1, 2, 3].includes(q.reason) ? q.reason : 2);
    } else if (q.type === "notify-action" && live.has(q.id)) {
      const n = live.get(q.id);
      const action = String(q.action);
      if (action === "default" || n.actions.some((a) => a[0] === action)) {
        dbus.emit(PATH, N, "ActivationToken", "us", [q.id, "rvwasm-notify-" + q.id]);
        dbus.emit(PATH, N, "ActionInvoked", "us", [q.id, action]);
        if (!(n.resident)) {
          closed(q.id, 2);
        }
      }
    } else if ((q.type === "tray-activate" || q.type === "tray-secondary" || q.type === "tray-scroll") && items.has(q.key)) {
      const it = items.get(q.key);
      const call = q.type === "tray-scroll"
        ? dbus.call(it.service, it.path, ITEM, "Scroll", "is", [q.delta | 0, q.orientation === "horizontal" ? "horizontal" : "vertical"])
        : dbus.call(it.service, it.path, ITEM, q.type === "tray-activate" ? "Activate" : "SecondaryActivate", "ii", [q.x | 0, q.y | 0]);
      call.catch((error) => log("notify: tray " + q.type + " " + q.key + ": " + error.message));
    } else if (q.type === "tray-list") {
      sendTray();
    }
  };
  ws.notify = { live, items };	// (for tests)
})
