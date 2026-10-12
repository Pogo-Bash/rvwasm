// The KDE services that are KWin's and plasmashell's, on the session bus
// (dbus.js), as far as System Settings and Plasma's command line tools use
// them. Written from their D-Bus interfaces and the code that calls them
// (KWin 6.3: src/org.kde.KWin.xml, src/org.kde.KWin.VirtualDesktopManager.xml,
// src/dbusinterface.cpp, src/kcms/desktop/desktopsmodel.cpp; plasma-workspace
// 6.3: shell/dbus/org.kde.PlasmaShell.xml, shell/shellcorona.cpp,
// kcms/wallpaper/wallpapermodule.cpp, wallpapers/image/plasma-apply-wallpaperimage.cpp).
//
// org.kde.KWin
//   /KWin org.kde.KWin: reconfigure (kwinrc and kwinrulesrc read again: the
//     desktops, the window rules), currentDesktop / setCurrentDesktop (1-based,
//     as X numbers them), nextDesktop, previousDesktop, showDesktop and
//     showingDesktop, supportInformation, activeOutputName, and the
//     reloadConfig signal (System Settings sends it after saving; we listen
//     for it as KWin does).
//   /VirtualDesktopManager org.kde.KWin.VirtualDesktopManager: the properties
//     count, current (an id), rows, navigationWrappingAround, desktops a(iss)
//     (position, id, name), the methods createDesktop, removeDesktop,
//     setDesktopName, and the signals countChanged, rowsChanged,
//     currentChanged, desktopCreated, desktopRemoved, desktopDataChanged,
//     navigationWrappingAroundChanged. That is all System Settings' Virtual
//     Desktops page uses (GetAll, then the methods, Set rows, and the signals).
// org.kde.plasmashell
//   /PlasmaShell org.kde.PlasmaShell: setWallpaper(plugin, parameters, screen)
//     and wallpaper(screen) (System Settings' Wallpaper page), evaluateScript
//     with the wallpaper part of Plasma's desktop scripting (what
//     plasma-apply-wallpaperimage sends: desktops(), wallpaperPlugin,
//     currentConfigGroup, writeConfig), the wallpaperChanged signal; the rest
//     of the interface answers but does nothing. The wallpaper is written into
//     ~/.config/plasma-org.kde.plasma.desktop-appletsrc as plasmashell keeps
//     it, which the page reads (plasma.js sends it on change) and draws.
//
// evaluateScript does not run the script: it comes from any program of the
// session, and this is a process of the page with the page's powers. It is
// read as tokens, and the statements that set wallpapers are carried out for
// every desktop containment (which is what the scripts that set wallpapers
// do: there is one screen here); a script with none of them is refused with
// an error saying so.
(function (ws) {
  "use strict";

  const { log, dbus } = ws;
  const K = ws.kconfig;
  if (!dbus || !dbus.connected) {
    return;
  }
  const { V, Fail } = dbus;

  // ---- org.kde.KWin

  const D = () => ws.desktops;
  const desktopData = (d, i) => [i, d.id, d.name];
  const VDM = "org.kde.KWin.VirtualDesktopManager";

  const reconfigure = () => {
    log("kwin: reconfigure (kwinrc, kwinrulesrc)");
    ws.desktopsReload();
    if (ws.rulesReload) {
      ws.rulesReload();
    }
  };

  dbus.export("/KWin", "org.kde.KWin", {
    methods: {
      reconfigure: { fn: reconfigure, noReply: true },
      cascadeDesktop: { fn: () => {}, noReply: true },
      unclutterDesktop: { fn: () => {}, noReply: true },
      killWindow: { fn: () => {}, noReply: true },
      currentDesktop: { out: "i", fn: () => D().current + 1 },
      setCurrentDesktop: { in: "i", out: "b", inNames: ["desktop"], fn: ([n]) => {
        if (n < 1 || n > D().list.length) {
          return false;
        }
        ws.setCurrentDesktop(n - 1);
        return true;
      } },
      nextDesktop: { fn: () => ws.setCurrentDesktop(ws.desktopStep(D().current, 5)) },
      previousDesktop: { fn: () => ws.setCurrentDesktop(ws.desktopStep(D().current, 6)) },
      stopActivity: { in: "s", out: "b", fn: () => false },
      startActivity: { in: "s", out: "b", fn: () => false },
      supportInformation: { out: "s", fn: () => "rvwasm wlserver: KWin's D-Bus interface for virtual desktops, window rules and "
        + "the wallpaper; " + D().list.length + " desktops, " + ws.windows.filter((w) => ws.isWindow(w)).length + " windows" },
      activeOutputName: { out: "s", fn: () => "PAGE-1" },
      showDebugConsole: { fn: () => {} },
      replace: { fn: () => {} },
      queryWindowInfo: { out: "a{sv}", annotations: { "org.qtproject.QtDBus.QtTypeName.Out0": "QVariantMap" },
                         fn: () => new Promise((resolve, reject) => {
        // The next press on a window picks it (seat.js), as KWin's interactive selection does.
        if (ws.picking) {
          ws.picking(null);
        }
        ws.picking = (w) => {
          if (!w) {
            reject(new Fail("org.kde.KWin.Error.UserCancel", "User cancelled the query"));
          } else if (!ws.isWindow(w)) {
            reject(new Fail("org.kde.KWin.Error.InvalidWindow", "Tried to query information about an unmanaged window"));
          } else {
            resolve(windowInfo(w));
          }
        };
        log("kwin: queryWindowInfo: the next click on a window picks it");
      }) },
      getWindowInfo: { in: "s", out: "a{sv}", annotations: { "org.qtproject.QtDBus.QtTypeName.Out0": "QVariantMap" }, fn: ([uuid]) => {
        const w = ws.windows.find((x) => ws.isWindow(x) && x.uuid === uuid);
        return w ? windowInfo(w) : {};
      } },
      showDesktop: { in: "b", inNames: ["showing"], noReply: true, fn: ([on]) => ws.setShowingDesktop(on) },
    },
    properties: {
      showingDesktop: { type: "b", get: () => D().showing },
    },
    signals: { reloadConfig: "", showingDesktopChanged: { type: "b", names: ["showing"] } },
  });

  dbus.export("/VirtualDesktopManager", VDM, {
    properties: {
      count: { type: "u", get: () => D().list.length },
      current: { type: "s", get: () => D().list[D().current].id, set: (id) => {
        const i = D().list.findIndex((d) => d.id === id);
        if (i >= 0) {
          ws.setCurrentDesktop(i);
        }
      } },
      rows: { type: "u", get: () => D().rows, set: (rows) => ws.setDesktopRows(rows) },
      navigationWrappingAround: { type: "b", get: () => D().wrap, set: (on) => {
        // (KWin keeps this in kwinrc [Windows] RollOverDesktops)
        const cfg = K.readFile("kwinrc");
        K.set(cfg, "Windows", "RollOverDesktops", on ? "true" : "false");
        K.write("kwinrc", cfg);
        ws.desktopsReload();
      } },
      desktops: { type: "a(iss)", get: () => D().list.map(desktopData) },
    },
    methods: {
      createDesktop: { in: "us", inNames: ["position", "name"], fn: ([position, name]) => {
        ws.createDesktop(position, name);
      } },
      setDesktopName: { in: "ss", inNames: ["id", "name"], fn: ([id, name]) => {
        ws.renameDesktop(id, name);
      } },
      removeDesktop: { in: "s", inNames: ["id"], fn: ([id]) => {
        ws.removeDesktop(id);
      } },
    },
    signals: {
      countChanged: { type: "u", names: ["count"] },
      rowsChanged: { type: "u", names: ["rows"] },
      currentChanged: { type: "s", names: ["id"] },
      navigationWrappingAroundChanged: { type: "b", names: ["navigationWrappingAround"] },
      desktopDataChanged: { type: "s(iss)", names: ["id", "desktopData"],
                            annotations: { "org.qtproject.QtDBus.QtTypeName.Out1": "KWin::DBusDesktopDataStruct" } },
      desktopCreated: { type: "s(iss)", names: ["id", "desktopData"],
                        annotations: { "org.qtproject.QtDBus.QtTypeName.Out1": "KWin::DBusDesktopDataStruct" } },
      desktopRemoved: { type: "s", names: ["id"] },
    },
  });

  // What KWin says of a window (src/dbusinterface.cpp, clientToVariantMap): the rules page fills its form with it.
  const windowInfo = (w) => {
    const t = w.top;
    if (!w.uuid) {
      const b = new Uint8Array(16);
      crypto.getRandomValues(b);
      b[6] = (b[6] & 0x0f) | 0x40;
      b[8] = (b[8] & 0x3f) | 0x80;
      const h = [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
      w.uuid = "{" + h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) + "-" + h.slice(16, 20) + "-" + h.slice(20) + "}";
    }
    const d = ws.rulesFor ? ws.rulesFor(w).d : { cls: w.appId, name: w.appId, title: w.title, role: "", type: "normal" };
    const g = ws.geometry(w);
    const dec = ws.decorated(w);
    const types = { normal: 0, dialog: 5, utility: 8, toolbar: 4, splash: 9 };
    const di = ws.windowDesktop(w);
    return [
      ["resourceClass", new V("s", d.cls)], ["resourceName", new V("s", d.name)], ["desktopFile", new V("s", "")],
      ["role", new V("s", d.role)], ["caption", new V("s", d.title)], ["clientMachine", new V("s", "localhost")],
      ["localhost", new V("b", true)], ["type", new V("i", types[d.type] || 0)],
      ["x", new V("i", t.wx - (dec ? chromeOf().border : 0))], ["y", new V("i", t.wy - (dec ? chromeOf().title + chromeOf().border : 0))],
      ["width", new V("i", g[2] + (dec ? 2 * chromeOf().border : 0))], ["height", new V("i", g[3] + (dec ? chromeOf().title + 2 * chromeOf().border : 0))],
      ["desktops", new V("as", di < 0 ? [] : [D().list[di].id])], ["minimized", new V("b", !!(w.flags & ws.F.MINIMIZED))],
      ["shaded", new V("b", !!t.shaded)], ["fullscreen", new V("b", !!(w.flags & ws.F.FULLSCREEN))],
      ["keepAbove", new V("b", !!t.above)], ["keepBelow", new V("b", !!t.below)], ["noBorder", new V("b", !!t.noBorder)],
      ["skipTaskbar", new V("b", !!t.skipTaskbar)], ["skipPager", new V("b", !!t.skipPager)], ["skipSwitcher", new V("b", !!t.skipSwitcher)],
      ["maximizeHorizontal", new V("b", !!(w.flags & ws.F.MAXIMIZED))], ["maximizeVertical", new V("b", !!(w.flags & ws.F.MAXIMIZED))],
      ["uuid", new V("s", w.uuid)], ["activities", new V("as", [])], ["layer", new V("i", t.above ? 4 : t.below ? 1 : 2)],
    ];
  };
  const chromeOf = () => ws.chrome;

  // desktops.js tells us what changed; listeners are told as KWin's VirtualDesktopManagerDBusInterface tells them.
  const emit = (member, sig, values) => dbus.emit("/VirtualDesktopManager", VDM, member, sig, values);
  ws.kwinDesktopsChanged = (kind, info) => {
    const list = D().list;
    switch (kind) {
    case "current":
      emit("currentChanged", "s", [list[D().current].id]);
      break;
    case "created":
      emit("desktopCreated", "s(iss)", [info.desktop.id, desktopData(info.desktop, info.position)]);
      // (the desktops after it moved one place on)
      for (let i = info.position + 1; i < list.length; i++) {
        emit("desktopDataChanged", "s(iss)", [list[i].id, desktopData(list[i], i)]);
      }
      emit("countChanged", "u", [list.length]);
      break;
    case "removed":
      for (let i = info.from; i < list.length; i++) {
        emit("desktopDataChanged", "s(iss)", [list[i].id, desktopData(list[i], i)]);
      }
      emit("desktopRemoved", "s", [info.desktop.id]);
      emit("countChanged", "u", [list.length]);
      break;
    case "renamed":
      emit("desktopDataChanged", "s(iss)", [info.desktop.id, desktopData(info.desktop, info.position)]);
      break;
    case "rows":
      emit("rowsChanged", "u", [D().rows]);
      break;
    case "showing":
      dbus.emit("/KWin", "org.kde.KWin", "showingDesktopChanged", "b", [D().showing]);
      break;
    case "reloaded": {
      const before = new Map(info.oldList.map((d, i) => [d.id, i]));
      list.forEach((d, i) => {
        if (!before.has(d.id)) {
          emit("desktopCreated", "s(iss)", [d.id, desktopData(d, i)]);
        } else if (before.get(d.id) !== i || info.oldList[before.get(d.id)].name !== d.name) {
          emit("desktopDataChanged", "s(iss)", [d.id, desktopData(d, i)]);
        }
      });
      for (const d of info.oldList) {
        if (!list.some((x) => x.id === d.id)) {
          emit("desktopRemoved", "s", [d.id]);
        }
      }
      if (info.oldList.length !== list.length) {
        emit("countChanged", "u", [list.length]);
      }
      if (info.oldRows !== D().rows) {
        emit("rowsChanged", "u", [D().rows]);
      }
      emit("currentChanged", "s", [list[D().current].id]);
      break;
    }
    }
  };

  // /Effects org.kde.kwin.Effects: KWin's effects are drawn by the page, which follows kwinrc [Plugins] NAMEEnabled
  // itself; System Settings' Desktop Effects page writes those keys and then asks to load or unload. Loaded is what
  // kwinrc says (an effect not named there counts as loaded); every effect is said to be supported, so that the page
  // lets the person choose.
  const effectsCfg = () => K.read("kwinrc");
  const effectOn = (name) => K.bool(effectsCfg(), "Plugins", name + "Enabled", true);
  const setEffect = (name, on) => {
    const cfg = K.readFile("kwinrc");
    K.set(cfg, "Plugins", name + "Enabled", on ? "true" : "false");
    K.write("kwinrc", cfg);
  };
  const named = () => [...(effectsCfg().groups.get("Plugins") || new Map()).keys()]
    .filter((k) => k.endsWith("Enabled")).map((k) => k.slice(0, -7));
  dbus.export("/Effects", "org.kde.kwin.Effects", {
    properties: {
      activeEffects: { type: "as", get: () => [] },
      loadedEffects: { type: "as", get: () => named().filter(effectOn) },
      listOfEffects: { type: "as", get: () => named() },
    },
    methods: {
      reconfigureEffect: { in: "s", inNames: ["name"], fn: () => {} },
      loadEffect: { in: "s", out: "b", inNames: ["name"], fn: ([name]) => {
        if (!effectOn(name)) {
          setEffect(name, true);
        }
        return true;
      } },
      toggleEffect: { in: "s", inNames: ["name"], fn: ([name]) => setEffect(name, !effectOn(name)) },
      unloadEffect: { in: "s", inNames: ["name"], fn: ([name]) => {
        if (effectOn(name)) {
          setEffect(name, false);
        }
      } },
      isEffectLoaded: { in: "s", out: "b", inNames: ["name"], fn: ([name]) => effectOn(name) },
      isEffectSupported: { in: "s", out: "b", inNames: ["name"], fn: () => true },
      areEffectsSupported: { in: "as", out: "ab", inNames: ["names"],
                             annotations: { "org.qtproject.QtDBus.QtTypeName.Out0": "QList<bool>" }, fn: ([names]) => names.map(() => true) },
      supportInformation: { in: "s", out: "s", inNames: ["name"], fn: ([name]) => name + ": drawn by the page" },
      debug: { in: "ss", out: "s", inNames: ["name", "parameter"], fn: () => "" },
    },
  });

  // System Settings saves, then says so: KWin reads its configuration again.
  dbus.match({ type: "signal", path: "/KWin", interface: "org.kde.KWin", member: "reloadConfig" }, () => reconfigure());

  dbus.own("org.kde.KWin");

  // ---- org.kde.plasmashell: the wallpaper

  const APPLETS = "plasma-org.kde.plasma.desktop-appletsrc";

  // The current activity: kactivitymanagerd's, from its file. If there is none yet, one is made there the way
  // kactivitymanagerd makes its first ("Default"), so that the desktop containment and kactivitymanagerd (which
  // System Settings' Wallpaper page starts, and asks which activity is current) agree.
  const activity = () => {
    const rc = K.readFile("kactivitymanagerdrc");
    let id = K.get(rc, "main", "currentActivity", "");
    if (!id) {
      const known = rc.groups.get("activities");
      id = known && known.size ? [...known.keys()][0] : null;
      if (!id) {
        const b = new Uint8Array(16);
        crypto.getRandomValues(b);
        b[6] = (b[6] & 0x0f) | 0x40;
        b[8] = (b[8] & 0x3f) | 0x80;
        const h = [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
        id = h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) + "-" + h.slice(16, 20) + "-" + h.slice(20);
        K.set(rc, "activities", id, "Default");
      }
      K.set(rc, "main", "currentActivity", id);
      K.write("kactivitymanagerdrc", rc);
      log("kwin: the current activity is " + id + " (written to kactivitymanagerdrc)");
    }
    return id;
  };

  // The desktop containments of appletsrc (not panels): [{ id, keys }].
  const desktopsOf = (cfg) => K.children(cfg, "Containments").map((id) => ({ id, keys: cfg.groups.get(K.group(["Containments", id])) }))
    .filter((c) => c.keys && c.keys.get("plugin") && c.keys.get("plugin") !== "org.kde.panel"
            && (c.keys.get("formfactor") || "0") === "0" && !/^[23]$/.test(c.keys.get("location") || "0"));

  // appletsrc with a desktop containment for the current activity on screen 0 (made as plasmashell makes its
  // first, if there is none); returns [cfg, containments, changed].
  const applets = () => {
    const cfg = K.readFile(APPLETS);
    const act = activity();
    let list = desktopsOf(cfg);
    let changed = false;
    if (!list.some((c) => c.keys.get("activityId") === act && (c.keys.get("lastScreen") || "0") === "0")) {
      const ids = K.children(cfg, "Containments").map(Number).filter((n) => !isNaN(n));
      const id = String(ids.length ? Math.max(...ids) + 1 : 1);
      const g = ["Containments", id];
      K.set(cfg, g, "activityId", act);
      K.set(cfg, g, "formfactor", "0");
      K.set(cfg, g, "immutability", "1");
      K.set(cfg, g, "lastScreen", "0");
      K.set(cfg, g, "location", "0");
      K.set(cfg, g, "plugin", "org.kde.plasma.folder");
      K.set(cfg, g, "wallpaperplugin", "org.kde.image");
      changed = true;
      list = desktopsOf(cfg);
      log("kwin: a desktop containment (" + id + ") for activity " + act + " in " + APPLETS);
    }
    return [cfg, list, changed];
  };

  // The configuration keys a wallpaper plugin has (its contents/config/main.xml), or null if it has none.
  const pluginKeys = (plugin) => {
    if (!/^[\w.-]+$/.test(plugin)) {
      return null;
    }
    for (const base of [K.home + "/.local/share/plasma/wallpapers/", "/usr/share/plasma/wallpapers/"]) {
      const xml = ws.readText(base + plugin + "/contents/config/main.xml");
      if (xml) {
        const groups = {};
        const re = /<group\s+name="([^"]*)"\s*>([\s\S]*?)<\/group>/g;
        let m;
        while ((m = re.exec(xml))) {
          const entries = {};
          const er = /<entry\s+name="([^"]*)"\s+type="([^"]*)"/g;
          let e;
          while ((e = er.exec(m[2]))) {
            entries[e[1]] = e[2];
          }
          groups[m[1]] = entries;
        }
        return groups;
      }
    }
    return null;
  };

  // A value from D-Bus (or a script) as KConfig writes it.
  const configText = (value, type) => {
    if (typeof value === "boolean") {
      return value ? "true" : "false";
    }
    if (Array.isArray(value)) {
      if (type === "Color" || (value.length === 1 && typeof value[0] === "number")) {
        const rgba = value[0] >>> 0;	// (a QColor from the Wallpaper page: a struct of its rgba)
        const a = rgba >>> 24, r = (rgba >>> 16) & 255, g = (rgba >>> 8) & 255, b = rgba & 255;
        return a === 255 ? r + "," + g + "," + b : r + "," + g + "," + b + "," + a;
      }
      return value.map((x) => String(x).replace(/\\/g, "\\\\").replace(/,/g, "\\,")).join(",");
    }
    if (value instanceof Uint8Array || (value && typeof value === "object")) {
      return null;
    }
    return String(value);
  };

  const wallpaperChanged = (screen) => {
    dbus.emit("/PlasmaShell", "org.kde.PlasmaShell", "wallpaperChanged", "u", [screen]);
    if (ws.plasmaAllow) {
      ws.plasmaAllow(currentImages());	// (the page may read the image wherever it is: plasma.js)
    }
  };

  // The images the desktop containments name, for plasma.js to let the page read them.
  const currentImages = () => {
    const cfg = K.readFile(APPLETS);
    const out = [];
    for (const c of desktopsOf(cfg)) {
      for (const plugin of ["org.kde.image", "org.kde.slideshow"]) {
        const img = K.get(cfg, ["Containments", c.id, "Wallpaper", plugin, "General"], "Image", null);
        if (img) {
          out.push(img.replace(/^file:\/\//, ""));
        }
      }
    }
    return out;
  };

  // Set keys of a plugin's group in every desktop containment on `screen` (null: all of them). values: [[key, value]].
  const writeWallpaper = (plugin, group, values, screen, setPlugin) => {
    const [cfg, list] = applets();
    const keys = pluginKeys(plugin);
    let n = 0;
    for (const c of list) {
      if (screen !== null && (c.keys.get("lastScreen") || "0") !== String(screen)) {
        continue;
      }
      if (setPlugin) {
        K.set(cfg, ["Containments", c.id], "wallpaperplugin", plugin);
      }
      for (const [key, value] of values) {
        const type = keys && keys[group[group.length - 1]] ? keys[group[group.length - 1]][key] : undefined;
        if (keys && keys[group[group.length - 1]] && type === undefined) {
          continue;	// (not one of the plugin's keys: plasmashell's KConfigPropertyMap ignores it too)
        }
        const text = configText(value, type);
        if (text !== null) {
          K.set(cfg, ["Containments", c.id, ...group], key, text);
        }
      }
      n++;
    }
    K.write(APPLETS, cfg);
    return n;
  };

  // ---- evaluateScript, the wallpaper part

  // The script's tokens: names, strings, numbers, punctuation (comments and spaces dropped). Null if it does not
  // read as JavaScript tokens.
  const tokens = (src) => {
    const out = [];
    let i = 0;
    while (i < src.length) {
      const c = src[i];
      if (/\s/.test(c)) {
        i++;
      } else if (src.startsWith("//", i)) {
        while (i < src.length && src[i] !== "\n") {
          i++;
        }
      } else if (src.startsWith("/*", i)) {
        const end = src.indexOf("*/", i + 2);
        i = end < 0 ? src.length : end + 2;
      } else if (c === "'" || c === "\"" || c === "`") {
        let s = "";
        i++;
        while (i < src.length && src[i] !== c) {
          if (src[i] === "\\" && i + 1 < src.length) {
            const e = src[i + 1];
            s += { n: "\n", t: "\t", r: "\r", "0": "\0" }[e] !== undefined ? { n: "\n", t: "\t", r: "\r", "0": "\0" }[e] : e;
            i += 2;
          } else {
            s += src[i++];
          }
        }
        if (i >= src.length) {
          return null;
        }
        i++;
        out.push({ t: "str", v: s });
      } else if (/[0-9]/.test(c)) {
        const m = /^[0-9.]+/.exec(src.slice(i));
        out.push({ t: "num", v: Number(m[0]) });
        i += m[0].length;
      } else if (/[A-Za-z_$]/.test(c)) {
        const m = /^[A-Za-z_$][\w$]*/.exec(src.slice(i));
        out.push({ t: "id", v: m[0] });
        i += m[0].length;
      } else {
        out.push({ t: "p", v: c });
        i++;
      }
    }
    return out;
  };

  // The wallpaper statements of a script, in order: ["plugin", name] | ["group", [names]] | ["write", key, value].
  const wallpaperStatements = (src) => {
    const t = tokens(src);
    if (!t) {
      return null;
    }
    const ops = [];
    const literal = (i) => {
      const x = t[i];
      if (!x) {
        return null;
      }
      if (x.t === "str" || x.t === "num") {
        return [x.v, i + 1];
      }
      if (x.t === "id" && (x.v === "true" || x.v === "false")) {
        return [x.v === "true", i + 1];
      }
      return null;
    };
    const list = (i) => {
      // [ 'a', 'b' ]  or  Array('a', 'b')  or  new Array('a', 'b')
      if (t[i] && t[i].t === "id" && t[i].v === "new") {
        i++;
      }
      let close = "]";
      if (t[i] && t[i].t === "id" && t[i].v === "Array" && t[i + 1] && t[i + 1].v === "(") {
        i += 2;
        close = ")";
      } else if (t[i] && t[i].v === "[") {
        i++;
      } else {
        return null;
      }
      const items = [];
      while (t[i] && t[i].v !== close) {
        const l = literal(i);
        if (!l) {
          return null;
        }
        items.push(String(l[0]));
        i = l[1];
        if (t[i] && t[i].v === ",") {
          i++;
        }
      }
      return t[i] ? [items, i + 1] : null;
    };
    for (let i = 0; i < t.length; i++) {
      if (t[i].v !== "." || !t[i + 1] || t[i + 1].t !== "id") {
        continue;
      }
      const name = t[i + 1].v;
      if (name === "wallpaperPlugin" && t[i + 2] && t[i + 2].v === "=" && t[i + 3] && t[i + 3].v !== "=") {
        const l = literal(i + 3);
        if (l && typeof l[0] === "string") {
          ops.push(["plugin", l[0]]);
        }
      } else if (name === "currentConfigGroup" && t[i + 2] && t[i + 2].v === "=" && t[i + 3] && t[i + 3].v !== "=") {
        const l = list(i + 3);
        if (l) {
          ops.push(["group", l[0]]);
        }
      } else if (name === "writeConfig" && t[i + 2] && t[i + 2].v === "(") {
        const k = literal(i + 3);
        if (k && t[k[1]] && t[k[1]].v === ",") {
          const v = literal(k[1] + 1);
          if (v && t[v[1]] && t[v[1]].v === ")") {
            ops.push(["write", String(k[0]), v[0]]);
          }
        }
      }
    }
    return ops;
  };

  const evaluateScript = (src) => {
    const ops = wallpaperStatements(String(src));
    if (!ops || !ops.length) {
      throw new Fail("org.kde.plasmashell.Error", "this desktop runs the wallpaper part of Plasma's scripting only "
                     + "(desktops(), wallpaperPlugin, currentConfigGroup, writeConfig)");
    }
    let plugin = null, group = null, done = 0;
    for (const op of ops) {
      if (op[0] === "plugin") {
        plugin = op[1];
        writeWallpaper(plugin, ["Wallpaper", plugin, "General"], [], null, true);
      } else if (op[0] === "group") {
        group = op[1];
      } else if (op[0] === "write") {
        // A containment's own keys (no group, or a group not under Wallpaper) are not ours to change here.
        const g = group || ["General"];
        if (g[0] !== "Wallpaper" || g.length < 2) {
          continue;
        }
        writeWallpaper(g[1], g.slice(), [[op[1], op[2]]], null, false);
        done++;
      }
    }
    log("kwin: evaluateScript: " + ops.length + " wallpaper statements (" + done + " values written)");
    wallpaperChanged(0);
    return "";
  };

  dbus.export("/PlasmaShell", "org.kde.PlasmaShell", {
    properties: {
      editMode: { type: "b", get: () => false, set: () => {} },
    },
    methods: {
      toggleDashboard: { fn: () => {} },
      toggleActivityManager: { fn: () => {} },
      toggleWidgetExplorer: { fn: () => {} },
      setDashboardShown: { in: "b", inNames: ["show"], fn: () => {} },
      evaluateScript: { in: "s", out: "s", inNames: ["script"], outNames: ["output"], fn: ([src]) => evaluateScript(src) },
      color: { out: "u", outNames: ["output"], fn: () => 0xff3daee9 },
      immutable: { out: "b", outNames: ["output"], fn: () => false },
      dumpCurrentLayoutJS: { out: "ay", outNames: ["script"], fn: () => new Uint8Array(0) },
      loadLookAndFeelDefaultLayout: { in: "s", inNames: ["layout"], fn: () => {} },
      activateLauncherMenu: { fn: () => {} },
      refreshCurrentShell: { fn: () => {} },
      wallpaper: { in: "u", out: "a{sv}", inNames: ["screenNum"], outNames: ["parameters"],
                   annotations: { "org.qtproject.QtDBus.QtTypeName.Out0": "QVariantMap" }, fn: ([screen]) => {
        const [cfg, list, changed] = applets();
        if (changed) {
          K.write(APPLETS, cfg);
        }
        const c = list.find((x) => (x.keys.get("lastScreen") || "0") === String(screen));
        if (!c) {
          return {};
        }
        const plugin = c.keys.get("wallpaperplugin") || "org.kde.image";
        const out = [["wallpaperPlugin", new V("s", plugin)]];
        const g = cfg.groups.get(K.group(["Containments", c.id, "Wallpaper", plugin, "General"]));
        for (const [k, v] of g || []) {
          out.push([k, new V("s", v)]);
        }
        return out;
      } },
      setWallpaper: { in: "sa{sv}u", inNames: ["wallpaperPlugin", "parameters", "screenNum"],
                      annotations: { "org.qtproject.QtDBus.QtTypeName.In1": "QVariantMap" }, fn: ([plugin, params, screen]) => {
        if (!plugin) {
          throw new Fail("org.freedesktop.DBus.Error.InvalidArgs", "setWallpaper: no wallpaper plugin");
        }
        const values = Object.entries(params || {}).filter(([k]) => k !== "PreviewImage");
        const n = writeWallpaper(plugin, ["Wallpaper", plugin, "General"], values, screen, true);
        log("kwin: setWallpaper " + plugin + " on screen " + screen + " (" + n + " containments): "
            + values.filter(([k]) => /^(Image|FillMode|Color)$/.test(k)).map(([k, v]) => k + "=" + configText(v, k === "Color" ? "Color" : "")).join(" "));
        wallpaperChanged(screen);
      } },
    },
    signals: {
      wallpaperChanged: { type: "u", names: ["screenNum"] },
      colorChanged: { type: "s", names: ["changedColor"] },
    },
  });

  // A desktop containment for the current activity from the start, so that System Settings' Wallpaper page finds
  // the one it looks for (it looks once, when kactivitymanagerd says which activity is current).
  // (Not while plasmashell is the desktop's shell: it makes its containments itself, and only with no
  // containment at all does it run its default layout, the one with the panel.)
  if (!ws.nativeShell) {
    const [cfg, , changed] = applets();
    if (changed) {
      K.write(APPLETS, cfg);
    }
  }
  if (ws.plasmaAllow) {
    ws.plasmaAllow(currentImages());
  }
  // (Not while plasmashell is the desktop's shell: it is org.kde.plasmashell then, and would not start were the
  // name ours: KDBusService's Unique.)
  if (!ws.nativeShell) {
    dbus.own("org.kde.plasmashell");
  }

  ws.kwin = { evaluateScript, wallpaperStatements, tokens };	// (for tests)
})
