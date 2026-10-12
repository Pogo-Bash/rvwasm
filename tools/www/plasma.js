// Plasma's look for the page's window chrome, configured the way Plasma is:
// by Plasma's own configuration files in the guest, and by the themes Plasma
// users install there.
//
// display.js draws the windows' frames, the panel, the launcher and the
// window switcher in the DOM over its canvas; this gives them KDE Plasma's
// look. Everything is drawn by the page: the guest runs nothing for it. What
// the page needs from the guest (configuration files, colour schemes, icons,
// fonts, cursors, wallpapers, window decoration and Plasma themes) it gets
// from the window server, which reads them for it, read-only and only where
// Plasma keeps them (wlserver/plasma.js), and which tells the page when the
// configuration changes: change the colour scheme, the fonts, the buttons'
// layout or the icon theme in System Settings, and the frames follow.
//
// What is read, and what it does here (each file as KConfig cascades it:
// /etc/xdg, then ~/.config/kdedefaults, then ~/.config; below them the
// Global Theme's contents/defaults, as Plasma's own startup puts there):
//   kdeglobals   [Colors:*] [WM] (the colour scheme, over the scheme file
//                [General] ColorScheme names), [General] font, fixed,
//                smallestReadableFont, [WM] activeFont, [Icons] Theme,
//                [KDE] AnimationDurationFactor, LookAndFeelPackage
//   kwinrc       [org.kde.kdecoration2] library, theme (Breeze, or an Aurorae
//                theme: __aurorae__svg__NAME), ButtonsOnLeft, ButtonsOnRight,
//                BorderSize, BorderSizeAuto; [Windows] TitlebarDoubleClickCommand
//   breezerc     [Windeco] ButtonSize, TitleAlignment, ShadowSize,
//                ShadowStrength, OutlineEnabled, DrawBorderOnMaximizedWindows
//   plasmarc     [Theme] name (the Plasma theme: panel, dialogs, task buttons)
//   kcminputrc   [Mouse] cursorTheme, cursorSize
//   plasma-org.kde.plasma.desktop-appletsrc, plasmashellrc   the panel (its
//                applets, the task manager's launchers, floating, thickness)
//                and the desktop's wallpaper
//
// Formats drawn: KDE colour schemes (.colors); KWin's Breeze decoration
// (drawn here in CSS after Breeze's design values: see "Breeze" below);
// Aurorae decorations (SVG themes: decoration.svg and the button SVGs, cut
// into their nine pieces by element id, as Plasma's FrameSvg does); Plasma
// themes (desktoptheme SVGs, the same way); freedesktop icon themes (looked up
// by the server, by the Icon Theme Specification); cursor themes (Breeze's
// scalable SVG cursors, or X cursor files); wallpaper packages and images.
//
// Not from KDE's code: no KDE source is copied. The numbers for Breeze are
// design values read in plasma/breeze (kdecoration/breeze.h,
// breezedecoration.cpp, breezebutton.cpp, breezesettingsdata.kcfg; GPL-2.0 or
// GPL-3.0) and plasma/kdecoration (decorationsettings.cpp); the colours are
// the guest's own colour scheme files (Breeze's are LGPL-2.0-or-later); icons,
// cursors, fonts and wallpapers are the guest's files, used where they are
// (Breeze icons LGPL-3.0-or-later, Noto Sans OFL-1.1, Hack MIT + Bitstream
// Vera, Plasma's wallpapers CC-BY-SA-4.0).
//
// ?chrome=plain: display.js does not load this; the plain frames stay.
// prefers-reduced-motion: no animations (as AnimationDurationFactor=0).
(() => {
  "use strict";

  // What happened, for whoever looks (window.rvPlasma.log; also the browser's console).
  const log = [];
  const note = (...parts) => {
    const line = parts.map((p) => (p && p.stack ? p.stack : String(p))).join(" ");
    console.log(line);
    log.push(line);
    if (log.length > 60) {
      log.shift();
    }
  };

  // ---- talking to the window server (wlserver/plasma.js)

  const channel = window.rvChannel("rv-plasma");
  const waiting = new Map();
  let nextId = 1;
  const host = () => window.rvDisplayHost || null;
  const ask = (q, ms = 20000) => new Promise((resolve) => {
    q.id = nextId++;
    waiting.set(q.id, resolve);
    channel.postMessage(q);
    const h = host();
    if (h && h.ringBell) {
      h.ringBell();	// (the server sees the question between two turns: the bell ends its wait)
    }
    setTimeout(() => {
      if (waiting.delete(q.id)) {
        resolve({ error: "no answer" });
      }
    }, ms);
  });
  channel.onmessage = (event) => {
    const m = event.data;
    if (!m || typeof m !== "object") {
      return;
    }
    if (m.id && waiting.has(m.id)) {
      const resolve = waiting.get(m.id);
      waiting.delete(m.id);
      resolve(m);
      if (m.type !== "config") {
        return;
      }
    }
    if (m.type === "config") {
      configured(m);
    } else if (m.type === "notify") {
      notify(m);
    } else if (m.type === "notify-close") {
      unnotify(m.id, 3, false);
    } else if (m.type === "tray") {
      // Programs' tray icons (the server is the StatusNotifierWatcher: PLASMA-PROTOCOL.md).
      trayItems = Array.isArray(m.items) ? m.items : [];
      const h = host();
      if (h && h.panelChanged) {
        h.panelChanged();
      }
    }
  };
  let trayItems = [];
  // A window server came up (its table's announcement): ask for the configuration.
  let serverPid = 0;
  const tables = window.rvChannel("rv-display");
  tables.onmessage = (event) => {
    const m = event.data;
    if (m && m.type === "table" && m.pid !== serverPid) {
      serverPid = m.pid;
      askConfig(m.pid);
      setTimeout(() => {
        channel.postMessage({ type: "tray-list" });
      }, 3000);
    }
  };

  // (asked again until it answers: a server that has only just announced its table may not be listening yet)
  const askConfig = async (pid) => {
    for (let i = 0; i < 30 && pid === serverPid; i++) {
      const r = await ask({ type: "config" }, 2000);
      if (!r.error) {
        return;
      }
    }
  };

  const decoder = new TextDecoder("utf-8");
  const fileCache = new Map();	// path -> Promise<Uint8Array | null>
  const fetchFile = (path) => {
    if (!fileCache.has(path)) {
      fileCache.set(path, ask({ type: "fetch", path }).then((r) => (r.data ? r.data : null)));
    }
    return fileCache.get(path);
  };
  const fetchText = async (path) => {
    let data = await fetchFile(path);
    if (data && path.endsWith("z") && data[0] === 0x1f && data[1] === 0x8b) {
      data = await gunzip(data);
    }
    return data ? decoder.decode(data) : null;
  };
  const listCache = new Map();
  const listDir = (path) => {
    if (!listCache.has(path)) {
      listCache.set(path, ask({ type: "list", path }).then((r) => r.entries || null));
    }
    return listCache.get(path);
  };
  const gunzip = async (data) => {
    try {
      const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream("gzip"));
      return new Uint8Array(await new Response(stream).arrayBuffer());
    } catch (error) {
      return null;
    }
  };
  // The first of some paths that exists, as text: [path, text], or [null, null].
  const firstText = async (paths) => {
    for (const p of paths) {
      const t = await fetchText(p);
      if (t !== null) {
        return [p, t];
      }
    }
    return [null, null];
  };

  // ---- KConfig files: groups ([A], [A][B] as "A][B"), keys (localized ones left out; [$e] expanded;
  // \s \t \n \r \\ unescaped). A cascade is the files merged in order, later ones winning.
  const parseConfig = (text, home) => {
    const groups = new Map();
    let g = null;
    if (!text) {
      return groups;
    }
    for (const raw of text.split("\n")) {
      const line = raw.replace(/\r$/, "").trim();
      if (!line || line[0] === "#") {
        continue;
      }
      if (line[0] === "[" && line.endsWith("]")) {
        const name = line.slice(1, -1);
        if (!groups.has(name)) {
          groups.set(name, {});
        }
        g = groups.get(name);
        continue;
      }
      const eq = line.indexOf("=");
      if (!g || eq <= 0) {
        continue;
      }
      let key = line.slice(0, eq).trim();
      let value = line.slice(eq + 1).trim();
      const flags = /\[\$([a-z]+)\]$/.exec(key);
      if (flags) {
        key = key.slice(0, flags.index);
        if (flags[1].includes("e")) {
          value = value.replace(/\$HOME|\$\{HOME\}/g, home || "/root");
        }
      }
      if (/\[[^\]]*\]$/.test(key)) {
        continue;	// (a translation)
      }
      g[key] = value.replace(/\\([stnr\\])/g, (m, c) => ({ s: " ", t: "\t", n: "\n", r: "\r", "\\": "\\" })[c]);
    }
    return groups;
  };
  const merge = (...configs) => {
    const out = new Map();
    for (const c of configs) {
      for (const [name, keys] of c) {
        out.set(name, Object.assign(out.get(name) || {}, keys));
      }
    }
    return out;
  };
  const read = (config, group, key, fallback) => {
    const g = config && config.get(group);
    return g && g[key] !== undefined && g[key] !== "" ? g[key] : fallback;
  };
  // (For a key whose empty value means something: KConfig only falls back to the default when the key is absent,
  // and "ButtonsOnRight=" is how a Global Theme with buttons on the left only says "none on the right".)
  const readKept = (config, group, key, fallback) => {
    const g = config && config.get(group);
    return g && g[key] !== undefined ? g[key] : fallback;
  };
  // A Global Theme's contents/defaults: groups "[file][group]" -> file -> config.
  const splitDefaults = (text) => {
    const byFile = new Map();
    for (const [name, keys] of parseConfig(text)) {
      const m = /^([^\]]+)\]\[(.+)$/.exec(name);
      // ("[Wallpaper]" alone is the wallpaper of the desktop; it goes to the appletsrc's place)
      const file = m ? m[1] : "_" + name;
      const group = m ? m[2] : name;
      if (!byFile.has(file)) {
        byFile.set(file, new Map());
      }
      byFile.get(file).set(group, keys);
    }
    return byFile;
  };

  // ---- colours

  const rgb = (value, fallback) => {
    if (!value) {
      return fallback;
    }
    const s = String(value).trim();
    let m = /^#([0-9a-f]{6})([0-9a-f]{2})?$/i.exec(s);
    if (m) {
      const n = parseInt(m[1], 16);
      return [n >> 16, (n >> 8) & 255, n & 255, m[2] ? parseInt(m[2], 16) / 255 : 1];
    }
    m = /^(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*(\d+))?$/.exec(s);
    if (m) {
      return [Number(m[1]), Number(m[2]), Number(m[3]), m[4] === undefined ? 1 : Number(m[4]) / 255];
    }
    return fallback;
  };
  const css = (c) => (c[3] === undefined || c[3] >= 1 ? "rgb(" + c.slice(0, 3).join(",") + ")" : "rgba(" + c.slice(0, 3).join(",") + "," + c[3].toFixed(3) + ")");
  const hex = (c) => "#" + c.slice(0, 3).map((x) => Math.round(x).toString(16).padStart(2, "0")).join("");
  const mix = (a, b, t) => [0, 1, 2].map((i) => Math.round(a[i] * (1 - t) + b[i] * t)).concat([1]);
  const luma = (c) => (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255;
  const alpha = (c, a) => [c[0], c[1], c[2], a];

  // Breeze Light's colours (the guest's BreezeLight.colors is read over these; these are what KColorScheme
  // falls back to when a file says nothing).
  const FALLBACK = {
    "Colors:Window": { BackgroundNormal: "239,240,241", BackgroundAlternate: "227,229,231", ForegroundNormal: "35,38,41",
                       ForegroundInactive: "112,125,138", ForegroundNegative: "218,68,83", ForegroundNeutral: "246,116,0",
                       ForegroundPositive: "39,174,96", DecorationFocus: "61,174,233", DecorationHover: "61,174,233" },
    "Colors:View": { BackgroundNormal: "255,255,255", ForegroundNormal: "35,38,41", DecorationHover: "61,174,233" },
    "Colors:Button": { BackgroundNormal: "252,252,252", ForegroundNormal: "35,38,41" },
    "Colors:Selection": { BackgroundNormal: "61,174,233", ForegroundNormal: "255,255,255" },
    "Colors:Tooltip": { BackgroundNormal: "247,247,247", ForegroundNormal: "35,38,41" },
    "Colors:Complementary": { BackgroundNormal: "42,46,50", ForegroundNormal: "252,252,252" },
    WM: { activeBackground: "227,229,231", activeForeground: "35,38,41", inactiveBackground: "239,240,241",
          inactiveForeground: "112,125,138" },
  };
  const schemeColors = (scheme) => {
    const c = (group, key, fallbackGroup = "Colors:Window") => rgb(read(scheme, group, key),
      rgb(read(scheme, fallbackGroup, key), rgb((FALLBACK[group] || {})[key], rgb(FALLBACK[fallbackGroup][key], [128, 128, 128, 1]))));
    const hasHeader = scheme.has("Colors:Header");
    const out = {
      winBg: c("Colors:Window", "BackgroundNormal"), winAlt: c("Colors:Window", "BackgroundAlternate"),
      winFg: c("Colors:Window", "ForegroundNormal"), winFgInactive: c("Colors:Window", "ForegroundInactive"),
      viewBg: c("Colors:View", "BackgroundNormal"), viewFg: c("Colors:View", "ForegroundNormal"),
      viewHover: c("Colors:View", "DecorationHover"), buttonBg: c("Colors:Button", "BackgroundNormal"),
      buttonFg: c("Colors:Button", "ForegroundNormal"), selBg: c("Colors:Selection", "BackgroundNormal"),
      selFg: c("Colors:Selection", "ForegroundNormal"), focus: c("Colors:Window", "DecorationFocus"),
      hover: c("Colors:Window", "DecorationHover"), negative: c("Colors:Window", "ForegroundNegative"),
      neutral: c("Colors:Window", "ForegroundNeutral"), positive: c("Colors:Window", "ForegroundPositive"),
      tipBg: c("Colors:Tooltip", "BackgroundNormal"), tipFg: c("Colors:Tooltip", "ForegroundNormal"),
      compBg: c("Colors:Complementary", "BackgroundNormal"), compFg: c("Colors:Complementary", "ForegroundNormal"),
      hasHeader,
    };
    // The title bar: the Header colours when the scheme has them (Plasma 5.25 and later; [Colors:Header][Inactive]
    // for inactive windows), else [WM] (KWin's decorationpalette.cpp).
    if (hasHeader) {
      out.titleBg = c("Colors:Header", "BackgroundNormal");
      out.titleFg = c("Colors:Header", "ForegroundNormal");
      out.titleBgI = rgb(read(scheme, "Colors:Header][Inactive", "BackgroundNormal"), out.titleBg);
      out.titleFgI = rgb(read(scheme, "Colors:Header][Inactive", "ForegroundNormal"), c("Colors:Header", "ForegroundInactive"));
    } else {
      out.titleBg = rgb(read(scheme, "WM", "activeBackground"), rgb(FALLBACK.WM.activeBackground));
      out.titleFg = rgb(read(scheme, "WM", "activeForeground"), rgb(FALLBACK.WM.activeForeground));
      out.titleBgI = rgb(read(scheme, "WM", "inactiveBackground"), rgb(FALLBACK.WM.inactiveBackground));
      out.titleFgI = rgb(read(scheme, "WM", "inactiveForeground"), rgb(FALLBACK.WM.inactiveForeground));
    }
    out.dark = luma(out.winBg) < 0.5;
    // Breeze's outline: the frame colour, between the background and the text (KColorScheme's frame contrast).
    out.outline = mix(out.titleBg, out.titleFg, out.dark ? 0.25 : 0.2);
    out.outlineI = mix(out.titleBgI, out.titleFgI, out.dark ? 0.2 : 0.15);
    return out;
  };

  // Plasma's stylesheet for SVGs (KSvg): the classes a theme's or an icon's SVG colours itself by.
  const colorSheet = (k) => {
    const rules = {
      "ColorScheme-Text": k.winFg, "ColorScheme-Background": k.winBg, "ColorScheme-Highlight": k.selBg,
      "ColorScheme-HighlightedText": k.selFg, "ColorScheme-ViewText": k.viewFg, "ColorScheme-ViewBackground": k.viewBg,
      "ColorScheme-ViewHover": k.viewHover, "ColorScheme-ViewFocus": k.focus, "ColorScheme-ButtonText": k.buttonFg,
      "ColorScheme-ButtonBackground": k.buttonBg, "ColorScheme-ButtonHover": k.hover, "ColorScheme-ButtonFocus": k.focus,
      "ColorScheme-NegativeText": k.negative, "ColorScheme-NeutralText": k.neutral, "ColorScheme-PositiveText": k.positive,
      "ColorScheme-ActiveText": k.focus, "ColorScheme-LinkText": k.focus, "ColorScheme-VisitedText": k.focus,
      "ColorScheme-ComplementaryText": k.compFg, "ColorScheme-ComplementaryBackground": k.compBg,
      "ColorScheme-ComplementaryHover": k.hover, "ColorScheme-ComplementaryFocus": k.focus,
      "ColorScheme-HeaderText": k.titleFg, "ColorScheme-HeaderBackground": k.titleBg,
      "ColorScheme-HeaderHover": k.hover, "ColorScheme-HeaderFocus": k.focus,
      "ColorScheme-ToolTipText": k.tipFg, "ColorScheme-ToolTipBackground": k.tipBg,
    };
    return Object.entries(rules).map(([cls, c]) => "." + cls + "{color:" + hex(c) + ";stop-color:" + hex(c) + "}").join("\n");
  };

  // ---- SVG from the guest: made safe to put into the page (it is measured in the DOM), recoloured.
  const sanitize = (doc) => {
    for (const bad of doc.querySelectorAll("script, foreignObject, iframe, object, embed, audio, video, a, image[href^='http'], animate, set")) {
      bad.remove();
    }
    for (const el of doc.querySelectorAll("*")) {
      for (const attr of [...el.attributes]) {
        const n = attr.name.toLowerCase(), v = attr.value.trim().toLowerCase();
        if (n.startsWith("on") || ((n === "href" || n === "xlink:href") && !v.startsWith("#") && !v.startsWith("data:image/"))
            || (n === "style" && /url\(\s*['"]?(?!#)/.test(v))) {
          el.removeAttribute(attr.name);
        }
      }
    }
    for (const style of doc.querySelectorAll("style")) {
      style.textContent = style.textContent.replace(/@import[^;]*;?/g, "").replace(/url\(\s*['"]?(?!#)[^)]*\)/g, "none");
    }
  };
  const parseSvg = (text, colors) => {
    if (!text) {
      return null;
    }
    const doc = new DOMParser().parseFromString(text, "image/svg+xml");
    const root = doc.documentElement;
    if (!root || root.nodeName.toLowerCase() !== "svg" || doc.querySelector("parsererror")) {
      return null;
    }
    sanitize(doc);
    if (colors) {
      const sheet = doc.getElementById("current-color-scheme");
      if (sheet) {
        sheet.textContent = colorSheet(colors);
      }
    }
    return doc;
  };
  const serialize = (doc) => new XMLSerializer().serializeToString(doc);
  // (The same picture, the same URL: a theme made again with nothing changed in a picture keeps its decoded image,
  // so the panel and the frames drawn again do not blink while new URLs decode.)
  const blobUrls = new Map();	// type + text -> url
  const blobUrl = (text, type = "image/svg+xml") => {
    const k = type + "\0" + text;
    let url = blobUrls.get(k);
    if (!url) {
      url = URL.createObjectURL(new Blob([text], { type }));
      blobUrls.set(k, url);
    }
    return url;
  };

  // ---- FrameSvg: a frame drawn from nine elements of an SVG (PREFIX-topleft, -top, -topright, -left, -center,
  // -right, -bottomleft, -bottom, -bottomright; no prefix: topleft, ...), with optional hint-*-margin elements
  // for the content's margins, as Plasma's themes and Aurorae decorations are made. Here the nine pieces are
  // put into one SVG image, three by three, and drawn with CSS border-image (corners at their size, the edges
  // and the middle stretched), so a frame is one element. Measured once in a hidden copy in the DOM.
  let measureBox = null;
  const measured = new Map();	// doc -> { root, ids: Map }
  const PIECES = ["topleft", "top", "topright", "left", "center", "right", "bottomleft", "bottom", "bottomright"];
  const attach = (doc) => {
    let m = measured.get(doc);
    if (m) {
      return m;
    }
    if (!measureBox) {
      measureBox = document.createElement("div");
      measureBox.style.cssText = "position:absolute;left:-10000px;top:0;width:0;height:0;overflow:hidden;visibility:hidden;pointer-events:none";
      document.body.appendChild(measureBox);
    }
    const root = document.importNode(doc.documentElement, true);
    const vb = (root.getAttribute("viewBox") || "").split(/[\s,]+/).map(Number);
    if (vb.length === 4 && vb.every((x) => isFinite(x))) {
      root.setAttribute("width", vb[2]);
      root.setAttribute("height", vb[3]);
    }
    // (ids are made unique per document: several SVGs are measured side by side)
    measureBox.appendChild(root);
    m = { root, vb: vb.length === 4 ? vb : [0, 0, 0, 0], boxes: new Map() };
    measured.set(doc, m);
    return m;
  };
  // An element's box, in the root's user units (viewBox), with the parent's transform for <use>; or null.
  const box = (doc, id) => {
    const m = attach(doc);
    if (m.boxes.has(id)) {
      return m.boxes.get(id);
    }
    let result = null;
    // (Several elements may have the id, Inkscape leaves such files; Qt's SVG renderer, which Plasma draws with,
    // takes the last one, and so does this.)
    const all = m.root.querySelectorAll("[id='" + id.replace(/'/g, "") + "']");
    const el = all.length ? all[all.length - 1] : null;
    if (el && typeof el.getBBox === "function") {
      try {
        const b = el.getBBox();
        const rootCtm = m.root.getScreenCTM();
        const ctm = el.getScreenCTM();
        if (rootCtm && ctm) {
          const toRoot = rootCtm.inverse().multiply(ctm);
          const pts = [[b.x, b.y], [b.x + b.width, b.y], [b.x, b.y + b.height], [b.x + b.width, b.y + b.height]].map(([x, y]) => {
            const p = new DOMPoint(x, y).matrixTransform(toRoot);
            return [p.x + m.vb[0], p.y + m.vb[1]];
          });
          const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
          const parent = el.parentNode && el.parentNode.getScreenCTM ? rootCtm.inverse().multiply(el.parentNode.getScreenCTM()) : new DOMMatrix();
          result = { x: Math.min(...xs), y: Math.min(...ys), w: Math.max(...xs) - Math.min(...xs), h: Math.max(...ys) - Math.min(...ys),
                     parent: new DOMMatrix([1, 0, 0, 1, m.vb[0], m.vb[1]]).multiply(parent) };
          if (!(result.w > 0 && result.h > 0)) {
            result = null;
          }
        }
      } catch (error) {
        result = null;
      }
    }
    m.boxes.set(id, result);
    return result;
  };
  // A copy of the SVG's content for <defs>, with every id in `ids` left on the last element that has it only
  // (as in Qt's SVG renderer, where the last element of an id is the one a name finds; a <use> finds that one,
  // not an earlier element of the same id). The ids themselves stay: themes make a mirrored corner as a <use> of
  // the other (decoration-topright = a flipped <use href="#decoration-topleft">), which a renamed id would orphan.
  const defsFor = (doc, out, ids) => {
    const src = doc.documentElement.cloneNode(true);
    for (const id of ids) {
      const all = src.querySelectorAll("[id='" + id.replace(/'/g, "") + "']");
      for (let i = 0; i < all.length - 1; i++) {
        all[i].removeAttribute("id");
      }
    }
    const defs = out.createElementNS("http://www.w3.org/2000/svg", "defs");
    for (const child of [...src.childNodes]) {
      defs.appendChild(out.importNode(child, true));
    }
    return defs;
  };
  const hasPrefix = (doc, prefix) => !!box(doc, (prefix ? prefix + "-" : "") + "center") || !!box(doc, (prefix ? prefix + "-" : "") + "topleft");
  // The frame of `prefix` as { url, slice: [t, r, b, l], margins: [t, r, b, l], center } or null.
  const frameCache = new Map();
  const frameSvg = (doc, prefix, key) => {
    const cacheKey = key + "|" + prefix;
    if (frameCache.has(cacheKey)) {
      return frameCache.get(cacheKey);
    }
    const p = prefix ? prefix + "-" : "";
    const b = {};
    for (const piece of PIECES) {
      b[piece] = box(doc, p + piece);
    }
    if (!b.center && !b.topleft && !b.top) {
      frameCache.set(cacheKey, null);
      return null;
    }
    const w = (...xs) => Math.max(0, ...xs.map((x) => (x ? x.w : 0)));
    const h = (...xs) => Math.max(0, ...xs.map((x) => (x ? x.h : 0)));
    const L = w(b.topleft, b.left, b.bottomleft), R = w(b.topright, b.right, b.bottomright);
    const C = Math.max(1, w(b.top, b.center, b.bottom));
    const T = h(b.topleft, b.top, b.topright), B = h(b.bottomleft, b.bottom, b.bottomright);
    const M = Math.max(1, h(b.left, b.center, b.right));
    const cells = { topleft: [0, 0, L, T], top: [L, 0, C, T], topright: [L + C, 0, R, T], left: [0, T, L, M], center: [L, T, C, M],
                    right: [L + C, T, R, M], bottomleft: [0, T + M, L, B], bottom: [L, T + M, C, B], bottomright: [L + C, T + M, R, B] };
    const ns = "http://www.w3.org/2000/svg";
    const out = document.implementation.createDocument(ns, "svg", null);
    const svg = out.documentElement;
    svg.setAttribute("xmlns:xlink", "http://www.w3.org/1999/xlink");
    svg.setAttribute("width", L + C + R);
    svg.setAttribute("height", T + M + B);
    svg.setAttribute("viewBox", "0 0 " + (L + C + R) + " " + (T + M + B));
    svg.appendChild(defsFor(doc, out, PIECES.filter((x) => b[x]).map((x) => p + x)));
    for (const piece of PIECES) {
      const e = b[piece], c = cells[piece];
      if (!e || c[2] <= 0 || c[3] <= 0) {
        continue;
      }
      const cell = out.createElementNS(ns, "svg");
      cell.setAttribute("x", c[0]);
      cell.setAttribute("y", c[1]);
      cell.setAttribute("width", c[2]);
      cell.setAttribute("height", c[3]);
      cell.setAttribute("viewBox", e.x + " " + e.y + " " + e.w + " " + e.h);
      cell.setAttribute("preserveAspectRatio", "none");
      const use = out.createElementNS(ns, "use");
      use.setAttribute("href", "#" + p + piece);
      const t = e.parent;
      use.setAttribute("transform", "matrix(" + [t.a, t.b, t.c, t.d, t.e, t.f].join(" ") + ")");
      cell.appendChild(use);
      svg.appendChild(cell);
    }
    const hint = (side) => {
      const x = box(doc, p + "hint-" + side + "-margin") || box(doc, "hint-" + side + "-margin");
      return x ? (side === "top" || side === "bottom" ? x.h : x.w) : null;
    };
    const margins = [hint("top"), hint("right"), hint("bottom"), hint("left")];
    const result = { url: blobUrl(serialize(out)), slice: [T, R, B, L], margins: margins.map((x, i) => (x === null ? [T, R, B, L][i] : x)),
                     center: !!b.center };
    frameCache.set(cacheKey, result);
    return result;
  };
  // The CSS for an element drawn as such a frame (border widths scaled by k).
  const frameCss = (f, fill = true, k = 1) => {
    if (!f) {
      return "";
    }
    const [t, r, b, l] = f.slice.map((x) => Math.round(x * k * 100) / 100);
    return "border-style:solid;border-width:" + t + "px " + r + "px " + b + "px " + l + "px;border-image-source:url(" + f.url
      + ");border-image-slice:" + f.slice.join(" ") + (fill && f.center ? " fill" : "") + ";border-image-width:" + t + "px " + r + "px "
      + b + "px " + l + "px;border-image-repeat:stretch;";
  };
  // A single element of an SVG (an Aurorae button's state, say) as an image: { url, w, h } or null.
  const elementSvg = (doc, id, key) => {
    const cacheKey = key + "|#" + id;
    if (frameCache.has(cacheKey)) {
      return frameCache.get(cacheKey);
    }
    const e = box(doc, id);
    let result = null;
    if (e) {
      const ns = "http://www.w3.org/2000/svg";
      const out = document.implementation.createDocument(ns, "svg", null);
      const svg = out.documentElement;
      svg.setAttribute("width", e.w);
      svg.setAttribute("height", e.h);
      svg.setAttribute("viewBox", e.x + " " + e.y + " " + e.w + " " + e.h);
      svg.setAttribute("preserveAspectRatio", "none");
      svg.appendChild(defsFor(doc, out, [id]));
      const use = out.createElementNS(ns, "use");
      use.setAttribute("href", "#" + id);
      const t = e.parent;
      use.setAttribute("transform", "matrix(" + [t.a, t.b, t.c, t.d, t.e, t.f].join(" ") + ")");
      svg.appendChild(use);
      result = { url: blobUrl(serialize(out)), w: e.w, h: e.h };
    }
    frameCache.set(cacheKey, result);
    return result;
  };

  // ---- the theme now: everything worked out from the configuration (rebuilt when it changes)

  let theme = null;		// see build()
  let serial = 0;
  let building = null, again = false;
  let lastFiles = null, home = "/root";
  const listeners = [];

  let lastJson = "";
  const configured = (m) => {
    const json = JSON.stringify(m.files || {});
    if (json === lastJson && theme) {
      return;	// (the same files again: nothing to do)
    }
    lastJson = json;
    if (theme) {
      const changed = Object.keys(m.files || {}).filter((f) => (m.files[f] || null) !== (lastFiles[f] || null));
      note("plasma: configuration changed: " + (changed.join(" ") || "(none)"));
    }
    lastFiles = m.files || {};
    home = m.home || "/root";
    if (building) {
      again = true;
      return;
    }
    building = build().catch((error) => note("plasma: " + (error && error.stack || error))).then(() => {
      building = null;
      if (again) {
        again = false;
        lastJson = "";
        configured({ files: lastFiles, home });
      }
    });
  };

  const cascade = (name, extra) => {
    const dirs = ["/etc/xdg", home + "/.config/kdedefaults", home + "/.config"];
    const layers = dirs.map((d) => parseConfig(lastFiles[d + "/" + name], home));
    return extra ? merge(layers[0], extra, layers[1], layers[2]) : merge(...layers);
  };

  const qtFont = (value, fallback) => {
    const parts = String(value || fallback).split(",");
    const family = parts[0] || "Noto Sans";
    let pt = Number(parts[1]);
    const px = Number(parts[2]) > 0 ? Number(parts[2]) : (pt > 0 ? pt : 10) * 96 / 72;
    let weight = Number(parts[4]) || 400;
    if (weight <= 99) {
      weight = weight >= 87 ? 900 : weight >= 75 ? 700 : weight >= 63 ? 600 : weight >= 57 ? 500 : weight >= 50 ? 400 : weight >= 25 ? 300 : 200;
    }
    return { family, px, weight, italic: parts[5] === "1" };
  };
  const fontCss = (f, generic = "sans-serif") => (f.italic ? "italic " : "") + f.weight + " " + f.px.toFixed(2) + "px \"" + f.family.replace(/"/g, "") + "\", \"Noto Sans\", " + generic;

  // Fonts from the guest: the family's regular and bold files, registered as FontFaces of the page.
  const fontsLoaded = new Map();
  let fontIndex = null;
  const fontFiles = async () => {
    if (fontIndex) {
      return fontIndex;
    }
    fontIndex = new Map();
    const walk = async (dir, depth) => {
      const entries = await listDir(dir);
      for (const [name, kind] of entries || []) {
        if (kind === "d" && depth < 3) {
          await walk(dir + "/" + name, depth + 1);
        } else if (/\.(ttf|otf)$/i.test(name)) {
          fontIndex.set(name.toLowerCase(), dir + "/" + name);
        }
      }
    };
    await walk("/usr/share/fonts", 0);
    await walk(home + "/.local/share/fonts", 1);
    return fontIndex;
  };
  const loadFont = async (family) => {
    if (fontsLoaded.has(family)) {
      return fontsLoaded.get(family);
    }
    const job = (async () => {
      const index = await fontFiles();
      const stem = family.replace(/\s+/g, "").toLowerCase();
      for (const [style, weight] of [["regular", 400], ["bold", 700]]) {
        const file = [...index.keys()].find((n) => n === stem + "-" + style + ".ttf" || n === stem + "-" + style + ".otf"
                                                   || (style === "regular" && (n === stem + ".ttf" || n === stem + ".otf")));
        if (!file) {
          continue;
        }
        const data = await fetchFile(index.get(file));
        if (data) {
          try {
            const face = new FontFace(family, data, { weight: String(weight) });
            await face.load();
            document.fonts.add(face);
          } catch (error) {
            note("plasma: font " + file + ": " + error.message);
          }
        }
      }
    })();
    fontsLoaded.set(family, job);
    return job;
  };

  // A theme's directory: the user's, else the system's.
  const findDir = async (kind, name) => {
    if (!name || name.includes("/") || name === "." || name === "..") {
      return null;
    }
    for (const base of [home + "/.local/share/" + kind, "/usr/share/" + kind]) {
      if (await listDir(base + "/" + name)) {
        return base + "/" + name;
      }
    }
    return null;
  };

  const build = async () => {
    const t0 = performance.now();
    // The Global Theme's defaults under everything (as Plasma's startup writes them into kdedefaults).
    const globals0 = cascade("kdeglobals");
    const lnfName = read(globals0, "KDE", "LookAndFeelPackage", "org.kde.breeze.desktop");
    const lnfDir = await findDir("plasma/look-and-feel", lnfName) || await findDir("plasma/look-and-feel", "org.kde.breeze.desktop");
    const defaults = lnfDir ? splitDefaults(await fetchText(lnfDir + "/contents/defaults")) : new Map();
    const files = {};
    for (const name of ["kdeglobals", "kwinrc", "breezerc", "auroraerc", "plasmarc", "kcminputrc"]) {
      files[name] = cascade(name, defaults.get(name));
    }
    files.appletsrc = cascade("plasma-org.kde.plasma.desktop-appletsrc");
    files.shellrc = cascade("plasmashellrc");
    const g = files.kdeglobals;

    // Colours: Breeze Light under the named scheme under kdeglobals' own groups.
    const schemeName = read(g, "General", "ColorScheme", "BreezeLight");
    const base = parseConfig((await fetchText("/usr/share/color-schemes/BreezeLight.colors")) || "");
    const [, named] = await firstText([home + "/.local/share/color-schemes/" + schemeName + ".colors",
                                       "/usr/share/color-schemes/" + schemeName + ".colors"]);
    const own = new Map([...g].filter(([name]) => name.startsWith("Colors:") || name === "WM"));
    const colors = schemeColors(merge(base, parseConfig(named || ""), own));

    // Fonts.
    const font = qtFont(read(g, "General", "font"), "Noto Sans,10,-1,5,400");
    const titleFont = qtFont(read(g, "WM", "activeFont"), read(g, "General", "font", "Noto Sans,10,-1,5,400"));
    const small = qtFont(read(g, "General", "smallestReadableFont"), "Noto Sans,8,-1,5,400");
    const fixed = qtFont(read(g, "General", "fixed"), "Hack,10,-1,5,400");
    await Promise.all([loadFont(font.family), loadFont(titleFont.family), loadFont(small.family)]);

    // Icons, cursors, animations.
    const iconTheme = read(g, "Icons", "Theme", "breeze");
    const reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    const anim = reduce ? 0 : Math.max(0, Math.min(4, Number(read(g, "KDE", "AnimationDurationFactor", "1")) || 0));
    const cursorTheme = read(files.kcminputrc, "Mouse", "cursorTheme", "breeze_cursors");
    const cursorSize = Math.max(16, Math.min(64, Number(read(files.kcminputrc, "Mouse", "cursorSize", "24")) || 24));

    // The window decoration.
    const deco = (key, d) => read(files.kwinrc, "org.kde.kdecoration2", key, d);
    const library = deco("library", "org.kde.breeze");
    const decoTheme = deco("theme", "Breeze");
    let decoration = { kind: "breeze" };
    if (library === "org.kde.kwin.aurorae" && decoTheme.startsWith("__aurorae__svg__")) {
      decoration = await aurorae(decoTheme.slice("__aurorae__svg__".length), colors) || { kind: "breeze", missing: decoTheme };
    }
    const bw = (key, d) => read(files.breezerc, "Windeco", key, d);
    const auto = deco("BorderSizeAuto", "true") !== "false";
    decoration.borderSize = auto ? (decoration.kind === "aurorae" ? "Normal" : "None") : deco("BorderSize", "Normal");
    // (Only kwinrc names the buttons, as KWin's decoration settings read it: a theme's own rc has no say, and a key
    // that is there but empty is an empty group, not the default. Look-and-feel packages write both keys into
    // kdedefaults/kwinrc, "ButtonsOnRight=" among them when they put every button on the left.)
    decoration.left = readKept(files.kwinrc, "org.kde.kdecoration2", "ButtonsOnLeft", "MS");
    decoration.right = readKept(files.kwinrc, "org.kde.kdecoration2", "ButtonsOnRight", "HIAX");
    decoration.doubleClick = read(files.kwinrc, "Windows", "TitlebarDoubleClickCommand", "Maximize");
    if (decoration.kind === "breeze") {
      const gridUnit = Math.max(2, 2 * Math.round(0.714 * titleFont.px / 2));
      const smallSpacing = Math.max(2, Math.floor(gridUnit / 4));
      const factor = { ButtonTiny: 1, ButtonSmall: 1.5, ButtonDefault: 2, ButtonLarge: 2.5, ButtonVeryLarge: 3.5 }[bw("ButtonSize", "ButtonDefault")] || 2;
      const steps = { None: 0, NoSides: 0, Tiny: 1, Normal: 2, Large: 3, VeryLarge: 4, Huge: 5, VeryHuge: 6, Oversized: 10 };
      Object.assign(decoration, {
        gridUnit, smallSpacing, button: Math.round(gridUnit * factor), radius: Math.round(2.5 * smallSpacing),
        border: (steps[decoration.borderSize] === undefined ? 2 : steps[decoration.borderSize]) * smallSpacing,
        noSides: decoration.borderSize === "NoSides",
        align: bw("TitleAlignment", "AlignCenterFullWidth"),
        outline: bw("OutlineEnabled", "true") !== "false",
        borderOnMaximized: bw("DrawBorderOnMaximizedWindows", "false") === "true",
        shadow: { ShadowNone: null, ShadowSmall: [4, 16, 1.0, 8, 0.4], ShadowMedium: [8, 32, 0.9, 16, 0.3],
                  ShadowLarge: [12, 48, 0.8, 24, 0.2], ShadowVeryLarge: [16, 64, 0.7, 32, 0.1] }[bw("ShadowSize", "ShadowLarge")],
        shadowStrength: Math.max(25, Math.min(255, Number(bw("ShadowStrength", "255")) || 255)) / 255,
      });
      if (decoration.borderSize === "NoSides") {
        decoration.border = Math.max(4, smallSpacing + decoration.radius);
      }
    }

    // KWin's effects that the page draws (kwinrc [Plugins] NAMEEnabled; Plasma 6's defaults: windows open and close
    // with Scale, minimize with Squash, desktops Slide, Maximize animates, popups fade).
    const plug = (name, d) => read(files.kwinrc, "Plugins", name + "Enabled", d ? "true" : "false") === "true";
    const effects = {
      open: plug("scale", true) || plug("glide", false) || plug("fade", false),
      minimize: plug("magiclamp", false) ? "magiclamp" : plug("squash", true) ? "squash" : null,
      slide: plug("slide", true), maximize: plug("maximize", true), popups: plug("fadingpopups", true),
    };

    // The Plasma theme (panel, dialogs, task buttons, list items).
    const plasmaTheme = await desktopTheme(read(files.plasmarc, "Theme", "name", "default"), colors);

    // The panel and the desktop.
    const panel = panelConfig(files.appletsrc, files.shellrc);
    const wallpaper = await wallpaperConfig(files.appletsrc, defaults.get("_Wallpaper"), colors);

    const next = {
      serial: ++serial, colors, font, titleFont, small, fixed, iconTheme, anim, cursorTheme, cursorSize, decoration, effects,
      plasmaTheme, panel, wallpaper, schemeName, lnfName,
    };
    // (icons are recoloured by the scheme: a new scheme, new pictures)
    if (!theme || theme.colors.winFg.join() !== colors.winFg.join() || theme.iconTheme !== iconTheme) {
      iconUrls.clear();
    }
    if (!theme || theme.cursorTheme !== cursorTheme || theme.cursorSize !== cursorSize) {
      cursors.clear();
      cursorQueue.clear();
    }
    theme = next;
    applyVars();
    loadCursors();
    note("plasma: theme " + serial + " (" + schemeName + ", " + (decoration.kind === "aurorae" ? "Aurorae " + decoration.name : "Breeze")
                + ", icons " + iconTheme + ", Plasma theme " + plasmaTheme.name + ") in " + Math.round(performance.now() - t0) + " ms");
    for (const f of listeners) {
      try {
        f(theme);
      } catch (error) {
        note(error);
      }
    }
    const h = host();
    if (h && h.themeChanged) {
      h.themeChanged();
    }
  };

  // CSS variables on the stage: the colours and fonts, for breeze.css.
  const applyVars = () => {
    const h = host();
    const stage = h && h.stage;
    if (!stage || !theme) {
      return;
    }
    const k = theme.colors;
    const v = {
      "--k-title-bg": css(k.titleBg), "--k-title-fg": css(k.titleFg), "--k-title-bg-i": css(k.titleBgI),
      "--k-title-fg-i": css(k.titleFgI), "--k-win-bg": css(k.winBg), "--k-win-alt": css(k.winAlt), "--k-win-fg": css(k.winFg),
      "--k-win-fg-inactive": css(k.winFgInactive), "--k-view-bg": css(k.viewBg), "--k-view-fg": css(k.viewFg),
      "--k-sel-bg": css(k.selBg), "--k-sel-fg": css(k.selFg), "--k-sel-bg-soft": css(alpha(k.selBg, 0.25)),
      "--k-hover": css(k.hover), "--k-hover-soft": css(alpha(k.hover, 0.2)), "--k-focus": css(k.focus),
      "--k-negative": css(k.negative), "--k-tip-bg": css(k.tipBg), "--k-tip-fg": css(k.tipFg),
      "--k-button-bg": css(k.buttonBg), "--k-button-fg": css(k.buttonFg),
      "--k-outline": css(k.outline), "--k-outline-i": css(k.outlineI), "--k-line": css(alpha(k.winFg, 0.2)),
      "--k-font": fontCss(theme.font), "--k-title-font": fontCss(theme.titleFont), "--k-small-font": fontCss(theme.small),
      "--k-font-size": theme.font.px.toFixed(2) + "px", "--k-anim": String(theme.anim),
    };
    for (const [name, value] of Object.entries(v)) {
      stage.style.setProperty(name, value);
    }
    stage.classList.add("rv-plasma");
    stage.classList.toggle("rv-plasma-dark", k.dark);
  };

  // ---- Aurorae decorations (KWin's SVG decoration engine: kwin/src/plugins/kdecorations/aurorae): a theme
  // directory with decoration.svg(z) (frames "decoration", "decoration-inactive", "decoration-maximized",
  // "decoration-maximized-inactive"), button SVGs (close, minimize, maximize, restore, alldesktops, keepabove,
  // keepbelow, shade, help; states "active", "inactive", "hover", "pressed", "deactivated", and "-inactive"
  // forms of hover and pressed) and NAMErc ([General] colours and alignment, [Layout] sizes).
  const aurorae = async (name, colors) => {
    const dir = await findDir("aurorae/themes", name);
    if (!dir) {
      return null;
    }
    const rc = parseConfig((await fetchText(dir + "/" + name + "rc")) || "");
    const svg = async (base) => {
      const [path, text] = await firstText([dir + "/" + base + ".svg", dir + "/" + base + ".svgz"]);
      return path ? parseSvg(text, colors) : null;
    };
    const decoration = await svg("decoration");
    if (!decoration) {
      return null;
    }
    const key = "aurorae:" + name + ":" + serial;
    const prefixes = ["decoration", "decoration-inactive", "decoration-maximized", "decoration-maximized-inactive"];
    const frames = {};
    for (const p of prefixes) {
      frames[p] = hasPrefix(decoration, p) ? frameSvg(decoration, p, key) : null;
    }
    frames["decoration-inactive"] = frames["decoration-inactive"] || frames.decoration;
    frames["decoration-maximized"] = frames["decoration-maximized"] || frames.decoration;
    frames["decoration-maximized-inactive"] = frames["decoration-maximized-inactive"] || frames["decoration-inactive"];
    const buttons = {};
    for (const b of ["close", "minimize", "maximize", "restore", "alldesktops", "keepabove", "keepbelow", "shade", "help"]) {
      const doc = await svg(b);
      if (!doc) {
        continue;
      }
      const states = {};
      for (const s of ["active", "inactive", "hover", "pressed", "deactivated", "hover-inactive", "pressed-inactive"]) {
        states[s] = hasPrefix(doc, s) ? (box(doc, s + "-topleft") ? { frame: frameSvg(doc, s, key + ":" + b) }
          : { image: elementSvg(doc, s + "-center", key + ":" + b) }) : null;
      }
      states.inactive = states.inactive || states.active;
      states["hover-inactive"] = states["hover-inactive"] || states.hover;
      states["pressed-inactive"] = states["pressed-inactive"] || states.pressed;
      buttons[b] = states;
    }
    const L = rc.get("Layout") || {}, G = rc.get("General") || {};
    const n = (k, d) => (L[k] === undefined || isNaN(Number(L[k])) ? d : Number(L[k]));
    const layout = {};
    for (const [k, d] of [["BorderLeft", 5], ["BorderRight", 5], ["BorderBottom", 5], ["TitleEdgeTop", 5], ["TitleEdgeBottom", 5],
                          ["TitleEdgeLeft", 5], ["TitleEdgeRight", 5], ["TitleBorderLeft", 5], ["TitleBorderRight", 5],
                          ["TitleHeight", 20], ["ButtonWidth", 20], ["ButtonHeight", 20], ["ButtonSpacing", 5], ["ButtonMarginTop", 0],
                          ["ExplicitButtonSpacer", 10], ["PaddingTop", 0], ["PaddingBottom", 0], ["PaddingLeft", 0], ["PaddingRight", 0],
                          ["TitleEdgeTopMaximized", null], ["TitleEdgeBottomMaximized", null], ["TitleEdgeLeftMaximized", null],
                          ["TitleEdgeRightMaximized", null], ["ButtonMarginTopMaximized", null]]) {
      layout[k] = n(k, d);
    }
    // (Each button may be wider or narrower than ButtonWidth: ButtonWidthClose and so on, as Aurorae's theme reads them.)
    layout.widths = {};
    for (const [kind, key] of [["minimize", "Minimize"], ["maximize", "MaximizeRestore"], ["close", "Close"], ["alldesktops", "Alldesktops"],
                               ["keepabove", "Keepabove"], ["keepbelow", "Keepbelow"], ["shade", "Shade"], ["help", "Help"],
                               ["menu", "Menu"], ["appmenu", "AppMenu"]]) {
      layout.widths[kind] = n("ButtonWidth" + key, layout.ButtonWidth);
    }
    // What is missing from the decoration's nine pieces (a corner that is not found is a hole in the frame: say so).
    for (const p of prefixes) {
      if (!/maximized/.test(p) && hasPrefix(decoration, p)) {
        const missing = PIECES.filter((x) => !box(decoration, p + "-" + x));
        if (missing.length) {
          note("plasma: aurorae " + name + ": " + p + " has no usable " + missing.join(", "));
        }
      }
    }
    return {
      kind: "aurorae", name, dir, frames, buttons, layout,
      activeText: rgb(G.ActiveTextColor, colors.titleFg), inactiveText: rgb(G.InactiveTextColor, colors.titleFgI),
      align: G.TitleAlignment || "Left", shadow: G.Shadow !== "false",
    };
  };

  // ---- Plasma themes (desktoptheme): a theme directory with widgets/ and dialogs/ SVGs, metadata (its fallback
  // theme), and maybe a colors file (a colour scheme of its own, else the system's colours are used).
  const desktopTheme = async (name, systemColors) => {
    const chain = [];
    for (let n = name, depth = 0; n && depth < 4 && !chain.some((c) => c.name === n); depth++) {
      const dir = await findDir("plasma/desktoptheme", n);
      if (!dir) {
        break;
      }
      let fallback = null;
      try {
        const meta = JSON.parse((await fetchText(dir + "/metadata.json")) || "{}");
        fallback = meta.X_Plasma_FallbackTheme || (meta.KPlugin && meta.KPlugin.X_Plasma_FallbackTheme) || null;
      } catch (error) {
        fallback = null;
      }
      if (!fallback) {
        const desktop = parseConfig((await fetchText(dir + "/metadata.desktop")) || "");
        fallback = read(desktop, "Desktop Entry", "X-Plasma-FallbackTheme", null);
      }
      chain.push({ name: n, dir });
      n = fallback || (n !== "default" ? "default" : null);
    }
    if (!chain.some((c) => c.name === "default")) {
      const dir = await findDir("plasma/desktoptheme", "default");
      if (dir) {
        chain.push({ name: "default", dir });
      }
    }
    const own = chain.length ? await fetchText(chain[0].dir + "/colors") : null;
    const colors = own ? schemeColors(merge(parseConfig((await fetchText("/usr/share/color-schemes/BreezeLight.colors")) || ""), parseConfig(own)))
      : systemColors;
    const docs = new Map();
    const doc = async (file) => {
      if (!docs.has(file)) {
        let d = null;
        for (const c of chain) {
          const [, text] = await firstText([c.dir + "/" + file + ".svgz", c.dir + "/" + file + ".svg"]);
          d = text ? parseSvg(text, colors) : null;
          if (d) {
            break;
          }
        }
        docs.set(file, d);
      }
      return docs.get(file);
    };
    const key = "desktoptheme:" + name + ":" + serial;
    const frame = async (file, prefixes) => {
      const d = await doc(file);
      if (!d) {
        return null;
      }
      for (const p of prefixes) {
        if (hasPrefix(d, p)) {
          return frameSvg(d, p, key + ":" + file);
        }
      }
      return null;
    };
    return {
      name: chain.length ? chain[0].name : name, colors, ownColors: !!own,
      panel: await frame("widgets/panel-background", ["floating", ""]),
      panelFlat: await frame("widgets/panel-background", ["south", ""]),
      panelEdges: { 3: await frame("widgets/panel-background", ["north", ""]), 4: await frame("widgets/panel-background", ["south", ""]),
                    5: await frame("widgets/panel-background", ["west", ""]), 6: await frame("widgets/panel-background", ["east", ""]) },
      panelShadow: await frame("widgets/panel-background", ["shadow"]),
      dialog: await frame("dialogs/background", [""]),
      dialogShadow: await frame("dialogs/background", ["shadow"]),
      tooltip: await frame("widgets/tooltip", [""]),
      task: {
        normal: await frame("widgets/tasks", ["normal"]), hover: await frame("widgets/tasks", ["hover"]),
        focus: await frame("widgets/tasks", ["focus"]), minimized: await frame("widgets/tasks", ["minimized"]),
        attention: await frame("widgets/tasks", ["attention"]),
      },
      item: {
        hover: await frame("widgets/viewitem", ["hover"]), selected: await frame("widgets/viewitem", ["selected+hover", "selected"]),
      },
      listItem: await frame("widgets/listitem", ["normal"]),	// (only its margins: Kickoff's rows are padded by them)
      toolButton: await frame("widgets/button", ["toolbutton-hover", "normal"]),	// (PlasmaComponents ToolButton: hover, padding)
      tab: await frame("widgets/tabbar", ["north-active-tab"]),	// (PlasmaComponents TabBar's current tab, TabButton's padding)
    };
  };

  // ---- the panel: plasma-org.kde.plasma.desktop-appletsrc's panel containment (plugin org.kde.panel), its
  // applets in AppletOrder, the task manager's launchers; plasmashellrc's thickness and floating. Without
  // those files: Plasma 6's default panel (plasma-desktop's layout-templates/org.kde.plasma.desktop.defaultPanel).
  const DEFAULT_APPLETS = ["org.kde.plasma.kickoff", "org.kde.plasma.pager", "org.kde.plasma.icontasks", "org.kde.plasma.marginsseparator",
                           "org.kde.plasma.systemtray", "org.kde.plasma.digitalclock", "org.kde.plasma.showdesktop"];
  const DEFAULT_LAUNCHERS = "applications:systemsettings.desktop,applications:org.kde.discover.desktop,preferred://filemanager,preferred://browser";
  // Every panel there is (several, on any edge: location 3 top, 4 bottom, 5 left, 6 right), in the order of their
  // containments; and Kickoff's icon and favourites (from the first launcher applet).
  const panelConfig = (applets, shell) => {
    const out = { panels: [], kickoffIcon: "start-here-kde-symbolic", fromFile: false,
                  favorites: "preferred://browser,org.kde.kontact.desktop,systemsettings.desktop,org.kde.dolphin.desktop,org.kde.discover.desktop".split(",") };
    let kickoffSeen = false;
    for (const [name, keys] of applets) {
      const m = /^Containments\]\[(\d+)$/.exec(name);
      if (!m || keys.plugin !== "org.kde.panel") {
        continue;
      }
      const panelId = m[1];
      out.fromFile = true;
      const p = { id: panelId, location: Number(keys.location) || 4, applets: [], launchers: DEFAULT_LAUNCHERS.split(","), labels: {} };
      const order = (read(applets, "Containments][" + panelId + "][General", "AppletOrder", "") || "").split(";").filter(Boolean);
      const found = [];
      for (const [gname, gkeys] of applets) {
        const a = new RegExp("^Containments\\]\\[" + panelId + "\\]\\[Applets\\]\\[(\\d+)$").exec(gname);
        if (a && gkeys.plugin) {
          found.push([a[1], gkeys.plugin]);
        }
      }
      found.sort((x, y) => (order.indexOf(x[0]) + 1 || 1e6 + Number(x[0])) - (order.indexOf(y[0]) + 1 || 1e6 + Number(y[0])));
      p.applets = found.map((x) => x[1]);
      for (const [id, plugin] of found) {
        const cfg = "Containments][" + panelId + "][Applets][" + id + "][Configuration][General";
        if (/taskmanager|icontasks/.test(plugin)) {
          const l = read(applets, cfg, "launchers", null);
          if (l !== null) {
            p.launchers = l.split(",").filter(Boolean);
          }
        }
        if (/kickoff|kicker|kickerdash/.test(plugin) && !kickoffSeen) {
          kickoffSeen = true;
          out.kickoffIcon = read(applets, cfg, "icon", out.kickoffIcon);
          const fav = read(applets, cfg, "favorites", read(applets, cfg, "favoriteApps", null));
          if (fav) {
            out.favorites = fav.split(",").filter(Boolean);
          }
        }
      }
      // plasmashellrc: [PlasmaViews][Panel N] floating, alignment (Qt: 1 left, 2 right, 4 or 132 centre),
      // panelLengthMode (0 fill, 1 fit the content, 2 custom), panelVisibility (0 always, 1 auto-hide, 2 dodge, 3
      // windows go below); [PlasmaViews][Panel N][Defaults] thickness, maxLength, offset.
      const view = "PlasmaViews][Panel " + panelId;
      p.thickness = Number(read(shell, view + "][Defaults", "thickness", read(shell, view, "thickness", "44"))) || 44;
      p.floating = read(shell, view, "floating", "1") !== "0";
      const align = Number(read(shell, view, "alignment", "4"));
      p.align = align & 1 ? "start" : align & 2 ? "end" : "center";
      p.lengthMode = Number(read(shell, view, "panelLengthMode", "0")) || 0;
      p.length = Number(read(shell, view + "][Defaults", "maxLength", read(shell, view + "][Defaults", "length", "0"))) || 0;
      p.offset = Number(read(shell, view + "][Defaults", "offset", "0")) || 0;
      p.visibility = Number(read(shell, view, "panelVisibility", "0")) || 0;
      out.panels.push(p);
    }
    if (!out.panels.length) {
      out.panels.push({ id: "default", location: 4, applets: DEFAULT_APPLETS.slice(), launchers: DEFAULT_LAUNCHERS.split(","),
                        thickness: 44, floating: true, align: "center", lengthMode: 0, length: 0, offset: 0, visibility: 0 });
    }
    for (const p of out.panels) {
      if (![3, 4, 5, 6].includes(p.location)) {
        p.location = 4;
      }
      p.vertical = p.location === 5 || p.location === 6;
      p.thickness = Math.max(24, Math.min(96, p.thickness));
      p.margin = p.floating ? 8 : 0;	// (a floating panel's gap to the screen's edges)
    }
    // (Kept for what reads the one panel: the first bottom panel, else the first.)
    const main = out.panels.find((p) => p.location === 4) || out.panels[0];
    Object.assign(out, { thickness: main.thickness, margin: main.margin, floating: main.floating, applets: main.applets,
                         launchers: main.launchers, location: main.location });
    return out;
  };

  // The strips the panels keep at each edge of the screen (an auto-hiding panel keeps none): { top, bottom, left, right }.
  const struts = () => {
    const out = { top: 0, bottom: 0, left: 0, right: 0 };
    if (!theme) {
      return out;
    }
    for (const p of theme.panel.panels) {
      if (p.visibility === 1 || p.visibility === 3) {
        continue;
      }
      const edge = { 3: "top", 4: "bottom", 5: "left", 6: "right" }[p.location];
      out[edge] = Math.max(out[edge], p.thickness + p.margin);
    }
    return out;
  };

  // ---- the wallpaper: the desktop containment's org.kde.image Image (a file, a directory, a package name), else
  // the Global Theme's [Wallpaper] Image (Next); of a package the image nearest the screen's size, from
  // contents/images_dark when the colour scheme is dark and the package has one.
  const wallpaperConfig = async (applets, lnfWallpaper, colors) => {
    let image = null, fill = 2, color = null;
    for (const [name, keys] of applets) {
      const m = /^Containments\]\[(\d+)$/.exec(name);
      if (m && keys.plugin && keys.plugin !== "org.kde.panel" && (keys.formfactor === undefined || keys.formfactor === "0")) {
        const general = applets.get("Containments][" + m[1] + "][Wallpaper][org.kde.image][General") || {};
        if (general.Image) {
          image = general.Image;
          fill = Number(general.FillMode || 2);
          color = rgb(general.Color, null);
          break;
        }
      }
    }
    if (!image) {
      image = lnfWallpaper && lnfWallpaper.Image || "Next";
    }
    image = image.replace(/^file:\/\//, "");
    let pkg = null;
    if (image[0] !== "/") {
      pkg = await findDir("wallpapers", image);
    } else if (await listDir(image.replace(/\/+$/, "") + "/contents")) {
      pkg = image.replace(/\/+$/, "");
    }
    if (pkg) {
      const sets = [];
      if (colors.dark) {
        sets.push(pkg + "/contents/images_dark");
      }
      sets.push(pkg + "/contents/images");
      for (const dir of sets) {
        const entries = await listDir(dir);
        if (entries && entries.length) {
          return { dir, images: entries.map((e) => e[0]).filter((n) => /\.(png|jpe?g|webp|avif)$/i.test(n)), fill, color };
        }
      }
      return null;
    }
    return /\.(png|jpe?g|webp|avif|svg)$/i.test(image) ? { file: image, fill, color } : null;
  };
  // The wallpaper as an ImageBitmap for a screen of w x h device pixels (cached by its path).
  let wallpaperNow = null;	// { path, bitmap, serial }
  let wallpaperJob = null;
  let wallpaperPick = { key: "", path: null };
  const wallpaper = (w, h) => {
    if (!theme || !theme.wallpaper) {
      return null;
    }
    const wp = theme.wallpaper;
    const key = theme.serial + ":" + w + "x" + h;
    let path = wallpaperPick.key === key ? wallpaperPick.path : wp.file || null;
    if (wallpaperPick.key !== key && !path && wp.images && wp.images.length) {
      const sized = wp.images.map((n) => {
        const m = /^(\d+)x(\d+)\./.exec(n);
        return m ? [Number(m[1]), Number(m[2]), n] : [0, 0, n];
      });
      const fits = sized.filter((x) => x[0] >= w && x[1] >= h).sort((a, b) => a[0] * a[1] - b[0] * b[1]
        || Math.abs(a[0] / a[1] - w / h) - Math.abs(b[0] / b[1] - w / h));
      const pick = fits[0] || sized.sort((a, b) => b[0] * b[1] - a[0] * a[1])[0];
      path = wp.dir + "/" + pick[2];
    }
    wallpaperPick = { key, path };	// (worked out once per theme and screen size: this runs every frame)
    if (!path) {
      return null;
    }
    if ((!wallpaperNow || wallpaperNow.path !== path) && (!wallpaperJob || wallpaperJob.path !== path)) {
      const job = { path };
      wallpaperJob = job;
      fetchFile(path).then(async (data) => {
        if (!data || wallpaperJob !== job) {
          return;
        }
        fileCache.delete(path);	// (the picture is kept as a bitmap; its bytes need not be)
        const bitmap = await createImageBitmap(new Blob([data]));
        if (wallpaperNow && wallpaperNow.bitmap) {
          wallpaperNow.bitmap.close();
        }
        wallpaperNow = { path, bitmap, serial: (wallpaperNow ? wallpaperNow.serial : 0) + 1, fill: wp.fill };
        wallpaperJob = null;
      }).catch((error) => note("plasma: wallpaper " + path + ": " + error.message));
    }
    return wallpaperNow;
  };

  // ---- icons: looked up by the server in the icon theme (and what it inherits, hicolor last); SVGs recoloured
  // with the colour scheme (KIconLoader does the same with the stylesheet Breeze's icons carry).
  const iconUrls = new Map();	// name@size -> Promise<url | null>
  const icon = (names, size = 32) => {
    if (!theme) {
      return Promise.resolve(null);
    }
    const list = (Array.isArray(names) ? names : [names]).filter(Boolean);
    const key = list.join("|") + "@" + size;
    if (!iconUrls.has(key)) {
      iconUrls.set(key, (async () => {
        const scale = Math.max(1, Math.min(3, Math.round(window.devicePixelRatio || 1)));
        for (const name of list) {
          const r = await ask({ type: "icon", name, size, scale, themes: [theme.iconTheme, "breeze"] });
          if (!r.data) {
            continue;
          }
          let data = r.data;
          if (/\.svgz$/.test(r.path)) {
            data = await gunzip(data);
          }
          if (/\.svgz?$/.test(r.path) && data) {
            const doc = parseSvg(decoder.decode(data), theme.colors);
            if (doc) {
              return blobUrl(serialize(doc));
            }
            continue;
          }
          return URL.createObjectURL(new Blob([data], { type: /\.png$/.test(r.path) ? "image/png" : "image/x-xpixmap" }));
        }
        return null;
      })());
    }
    return iconUrls.get(key);
  };
  // An <img> that shows an icon when it has come.
  const iconImg = (names, size, cls) => {
    const img = document.createElement("img");
    img.className = cls || "rv-icon";
    img.width = size;
    img.height = size;
    img.alt = "";
    img.draggable = false;
    img.style.visibility = "hidden";
    icon(names, size).then((url) => {
      if (url) {
        img.src = url;
        img.style.visibility = "";
      }
    });
    return img;
  };

  // The program a window belongs to (its app id against the launcher's .desktop files), and its icon names.
  const appFor = (appId) => {
    const h = host();
    const apps = h && h.apps ? h.apps() : [];
    if (!appId) {
      return null;
    }
    const id = appId.toLowerCase();
    return apps.find((a) => a.file && a.file.toLowerCase() === id + ".desktop")
      || apps.find((a) => a.wmclass && a.wmclass.toLowerCase() === id)
      || apps.find((a) => a.file && a.file.toLowerCase().replace(/\.desktop$/, "").split(".").pop() === id.split(".").pop())
      || apps.find((a) => a.icon && a.icon.toLowerCase() === id)
      // (KDE programs whose window says "kdeNAME": System Settings is kdesystemsettings)
      || (id.startsWith("kde") ? apps.find((a) => a.file && a.file.toLowerCase() === id.slice(3) + ".desktop") : null) || null;
  };
  const appIcons = (appId) => {
    const a = appFor(appId);
    return [a && a.icon, appId, appId && appId.split(".").pop(), "application-x-executable"].filter(Boolean);
  };
  // A launcher entry of the panel (applications:NAME.desktop, preferred://filemanager|browser, file:///...desktop).
  const launcherApp = (url) => {
    const h = host();
    const apps = h && h.apps ? h.apps() : [];
    const byFile = (f) => apps.findIndex((a) => a.file === f);
    let i = -1;
    if (url === "preferred://filemanager") {
      i = byFile("org.kde.dolphin.desktop");
    } else if (url === "preferred://browser") {
      for (const f of ["firefox-esr.desktop", "firefox.desktop", "chromium.desktop", "org.kde.falkon.desktop", "org.kde.konqueror.desktop"]) {
        i = byFile(f);
        if (i >= 0) {
          break;
        }
      }
    } else {
      i = byFile(url.replace(/^applications:/, "").replace(/^file:\/\/.*\//, ""));
    }
    return i >= 0 ? { index: i, app: apps[i] } : null;
  };

  // ---- cursors: kcminputrc's theme (and what its index.theme inherits); Breeze's scalable cursors
  // (cursors_scalable/NAME/metadata.json and SVGs, Plasma 6.2 and later), else X cursor files (cursors/NAME).
  // CSS cursors by name, with the X cursor names themes use for them.
  const CURSOR_NAMES = {
    default: ["default", "left_ptr", "arrow"], "context-menu": ["context-menu", "left_ptr"], help: ["help", "question_arrow", "whats_this"],
    pointer: ["pointer", "pointing_hand", "hand2", "hand1"], progress: ["progress", "left_ptr_watch", "half-busy"], wait: ["wait", "watch"],
    cell: ["cell", "plus"], crosshair: ["crosshair", "cross", "tcross"], text: ["text", "xterm", "ibeam"], "vertical-text": ["vertical-text"],
    alias: ["alias", "dnd-link", "link"], copy: ["copy", "dnd-copy"], move: ["move", "fleur", "size_all"], "no-drop": ["no-drop", "forbidden"],
    "not-allowed": ["not-allowed", "forbidden", "crossed_circle"], grab: ["grab", "openhand"], grabbing: ["grabbing", "closedhand", "dnd-move"],
    "e-resize": ["e-resize", "right_side"], "n-resize": ["n-resize", "top_side"], "ne-resize": ["ne-resize", "top_right_corner"],
    "nw-resize": ["nw-resize", "top_left_corner"], "s-resize": ["s-resize", "bottom_side"], "se-resize": ["se-resize", "bottom_right_corner"],
    "sw-resize": ["sw-resize", "bottom_left_corner"], "w-resize": ["w-resize", "left_side"], "ew-resize": ["ew-resize", "size_hor", "h_double_arrow"],
    "ns-resize": ["ns-resize", "size_ver", "v_double_arrow"], "nesw-resize": ["nesw-resize", "size_bdiag"], "nwse-resize": ["nwse-resize", "size_fdiag"],
    "col-resize": ["col-resize", "split_h", "sb_h_double_arrow"], "row-resize": ["row-resize", "split_v", "sb_v_double_arrow"],
    "all-scroll": ["all-scroll", "fleur"], "zoom-in": ["zoom-in"], "zoom-out": ["zoom-out"],
  };
  const cursors = new Map();	// css name -> css value
  const cursorQueue = new Map();
  let cursorDirs = null;
  const loadCursors = async () => {
    if (!theme) {
      return;
    }
    const mine = theme;
    // The theme's directories that have cursors, following Inherits.
    const dirs = [];
    const visit = async (name, depth) => {
      if (depth > 4 || dirs.some((d) => d.name === name)) {
        return;
      }
      for (const base of [home + "/.local/share/icons", home + "/.icons", "/usr/share/icons"]) {
        const d = base + "/" + name;
        const entries = await listDir(d);
        if (!entries) {
          continue;
        }
        const names = entries.map((e) => e[0]);
        if (names.includes("cursors") || names.includes("cursors_scalable")) {
          dirs.push({ name, dir: d, scalable: names.includes("cursors_scalable") });
        }
        const index = parseConfig((await fetchText(d + "/index.theme")) || "");
        for (const i of (read(index, "Icon Theme", "Inherits", "") || "").split(",").map((x) => x.trim()).filter(Boolean)) {
          await visit(i, depth + 1);
        }
        break;
      }
    };
    await visit(theme.cursorTheme, 0);
    if (mine !== theme) {
      return;
    }
    cursorDirs = dirs;
    const h = host();
    for (const name of Object.keys(CURSOR_NAMES)) {
      const value = await cursorFor(name);
      if (mine !== theme) {
        return;
      }
      if (value) {
        cursors.set(name, value);
        if (h && h.stage) {
          h.stage.style.setProperty("--cur-" + name, value);
        }
      }
    }
    if (h && h.cursorsChanged) {
      h.cursorsChanged();
    }
  };
  const cursorFor = async (css) => {
    const size = theme.cursorSize, dpr = Math.max(1, Math.min(3, window.devicePixelRatio || 1));
    for (const d of cursorDirs || []) {
      for (const name of CURSOR_NAMES[css] || [css]) {
        if (d.scalable) {
          let meta = null;
          try {
            meta = JSON.parse((await fetchText(d.dir + "/cursors_scalable/" + name + "/metadata.json")) || "null");
          } catch (error) {
            meta = null;
          }
          if (Array.isArray(meta) && meta.length) {
            const m = meta[0];
            const svg = await fetchText(d.dir + "/cursors_scalable/" + name + "/" + m.filename);
            if (svg) {
              const nominal = m.nominal_size || 24;
              const k = size / nominal;
              const url = await rasterize(svg, Math.round(nominal * k * dpr));
              if (url) {
                const hx = Math.round((m.hotspot_x || 0) * k), hy = Math.round((m.hotspot_y || 0) * k);
                return "image-set(url(" + url + ") " + dpr + "x) " + hx + " " + hy + ", " + css;
              }
            }
          }
        }
        const x = await fetchFile(d.dir + "/cursors/" + name);
        if (x) {
          const c = xcursor(x, Math.round(size * dpr));
          if (c) {
            return "image-set(url(" + c.url + ") " + (c.size / size).toFixed(3) + "x) " + Math.round(c.hx * size / c.size) + " "
              + Math.round(c.hy * size / c.size) + ", " + css;
          }
        }
      }
    }
    return null;
  };
  const rasterize = async (svgText, px) => {
    const doc = parseSvg(svgText, null);
    if (!doc) {
      return null;
    }
    const img = new Image();
    img.src = blobUrl(serialize(doc));
    try {
      await img.decode();
    } catch (error) {
      return null;
    }
    const c = document.createElement("canvas");
    c.width = c.height = Math.max(8, Math.min(128, px));
    c.getContext("2d").drawImage(img, 0, 0, c.width, c.height);
    return c.toDataURL("image/png");
  };
  // An X cursor file (libXcursor's format: "Xcur", a table of chunks; image chunks 0xfffd0002 with nominal size,
  // width, height, hot spot, delay and ARGB pixels): the image of the nominal size nearest `want`, as a PNG.
  const xcursor = (b, want) => {
    const v = new DataView(b.buffer, b.byteOffset, b.length);
    if (b.length < 16 || v.getUint32(0, true) !== 0x72756358) {
      return null;
    }
    const n = v.getUint32(8, true);
    let best = null;
    for (let i = 0; i < n && 16 + 12 * i + 12 <= b.length; i++) {
      const type = v.getUint32(16 + 12 * i, true), sub = v.getUint32(20 + 12 * i, true), pos = v.getUint32(24 + 12 * i, true);
      if (type === 0xfffd0002 && (!best || Math.abs(sub - want) < Math.abs(best.sub - want))) {
        best = { sub, pos };
      }
    }
    if (!best || best.pos + 36 > b.length) {
      return null;
    }
    const at = best.pos;
    const w = v.getUint32(at + 16, true), h = v.getUint32(at + 20, true), hx = v.getUint32(at + 24, true), hy = v.getUint32(at + 28, true);
    if (!w || !h || w > 256 || h > 256 || at + 36 + w * h * 4 > b.length) {
      return null;
    }
    const c = document.createElement("canvas");
    c.width = w;
    c.height = h;
    const ctx = c.getContext("2d");
    const image = ctx.createImageData(w, h);
    for (let p = 0; p < w * h; p++) {
      const argb = v.getUint32(at + 36 + 4 * p, true);
      const a = argb >>> 24;
      // (premultiplied ARGB to straight RGBA)
      image.data[4 * p] = a ? Math.min(255, ((argb >> 16) & 255) * 255 / a) : 0;
      image.data[4 * p + 1] = a ? Math.min(255, ((argb >> 8) & 255) * 255 / a) : 0;
      image.data[4 * p + 2] = a ? Math.min(255, (argb & 255) * 255 / a) : 0;
      image.data[4 * p + 3] = a;
    }
    ctx.putImageData(image, 0, 0);
    return { url: c.toDataURL("image/png"), size: best.sub, hx, hy };
  };
  const cursor = (name) => cursors.get(name) || name;

  // ---- window frames
  //
  // display.js makes a frame per decorated window (.rv-frame, as big as the window with its title bar and the
  // border the server keeps, see wlserver/README.md) with a title element it drags by and edges it resizes by;
  // decorateFrame fills the title (buttons in kwinrc's order, the title text) and adds the decoration (.rv-deco:
  // Breeze's in CSS, or an Aurorae theme's SVG frame), layoutFrame places them for the window's state, and says
  // how far the decoration reaches out of the frame (its shadow), which display.js clips like the frame.

  // Breeze's button glyphs, drawn on a 20-unit square (breezebutton.cpp's geometry, our own SVG).
  const GLYPHS = {
    X: "<path d='M6 6L14 14M14 6L6 14'/>",
    I: "<path d='M5 8L10 13L15 8'/>",
    A: "<path d='M5 12L10 7L15 12'/>",
    R: "<path d='M5 10L10 5L15 10L10 15Z'/>",
    F: "<path d='M5 10L10 5L15 10M5 14L10 9L15 14'/>",
    B: "<path d='M5 6L10 11L15 6M5 10L10 15L15 10'/>",
    L: "<path d='M5 6.5H15M5 14L10 9L15 14'/>",
    S: "<circle cx='10' cy='10' r='2.6' fill='currentColor' stroke='none'/>",
    H: "<path d='M7.5 8a2.5 2.5 0 1 1 3.5 2.3c-.7.3-1 .8-1 1.5V13M10 15.5v.5'/>",
    N: "<path d='M4.5 6.5H15.5M4.5 10H15.5M4.5 13.5H15.5'/>",
  };
  const glyph = (letter) => "<svg viewBox='0 0 20 20' width='100%' height='100%' fill='none' stroke='currentColor' stroke-width='1.1' stroke-linecap='round' stroke-linejoin='round'>"
    + (GLYPHS[letter] || "") + "</svg>";
  const BUTTONS = { M: "menu", S: "alldesktops", H: "help", I: "minimize", A: "maximize", X: "close", F: "keepabove", B: "keepbelow",
                    L: "shade", N: "appmenu", E: "exclude", _: "spacer" };
  // The buttons the page can carry out (the others need things this window server does not have: virtual
  // desktops, keep above/below, shading, context help, the application menu, screen capture rules).
  const WORKING = new Set(["M", "I", "A", "X", "_"]);
  const WORKING_SHELL = new Set(["M", "I", "A", "X", "_", "F", "B", "L", "S"]);	// (with the Plasma shell's server)

  const decorateFrame = (f, id, api) => {
    if (!theme) {
      return;
    }
    f.theme = frameKey(api.shell().version);
    const working = api.shell().version ? WORKING_SHELL : WORKING;
    const d = theme.decoration;
    f.el.classList.add("rv-plasma-frame", d.kind === "aurorae" ? "rv-aurorae" : "rv-breeze");
    const deco = document.createElement("div");
    deco.className = "rv-deco";
    f.el.insertBefore(deco, f.el.firstChild);
    f.deco = deco;
    const title = f.title;
    for (const b of [...title.querySelectorAll(".rv-btn")]) {
      b.remove();
    }
    const left = document.createElement("div");
    left.className = "rv-btns rv-btns-left";
    const right = document.createElement("div");
    right.className = "rv-btns rv-btns-right";
    title.insertBefore(left, title.firstChild);
    title.appendChild(right);
    f.text.classList.add("rv-caption");
    f.buttons = [];
    const make = (letter) => {
      if (!working.has(letter)) {
        return null;
      }
      const kind = BUTTONS[letter];
      const b = document.createElement("button");
      b.className = "rv-pbtn rv-pbtn-" + kind;
      b.tabIndex = -1;
      b.dataset.kind = kind;
      b.title = { menu: "Window menu", minimize: "Minimize", maximize: "Maximize", close: "Close", spacer: "" }[kind] || kind;
      if (kind === "close") {
        b.innerHTML = glyph("X");
      } else if (kind === "minimize") {
        b.innerHTML = glyph("I");
      } else if (letter === "F" || letter === "B" || letter === "L" || letter === "S") {
        b.innerHTML = glyph(letter);
      } else if (kind === "maximize") {
        b.innerHTML = "<span class='rv-g-a'>" + glyph("A") + "</span><span class='rv-g-r'>" + glyph("R") + "</span>";
      }
      if (kind === "spacer") {
        b.disabled = true;
      } else {
        b.addEventListener("pointerdown", (event) => {
          event.stopPropagation();
          event.preventDefault();
          api.focusScreen();
        });
        b.addEventListener("click", (event) => {
          event.stopPropagation();
          if (kind === "close") {
            api.order("close", id);
          } else if (kind === "minimize") {
            api.order("minimize", id);
          } else if (kind === "maximize") {
            api.order("maximize", id);
          } else if (kind === "menu") {
            windowMenu(f, id, api, b);
          } else if (kind === "keepabove") {
            api.raw(11, id, 2);	// (PLASMA-PROTOCOL.md: 11 keep above, 12 below, 13 shade, 14 all desktops; 2 toggles)
          } else if (kind === "keepbelow") {
            api.raw(12, id, 2);
          } else if (kind === "shade") {
            api.raw(13, id, 2);
          } else if (kind === "alldesktops") {
            api.raw(14, id, 2);
          }
        });
        b.addEventListener("dblclick", (event) => event.stopPropagation());
      }
      f.buttons.push({ letter, kind, el: b });
      return b;
    };
    for (const letter of d.left) {
      const b = make(letter);
      if (b) {
        left.appendChild(b);
      }
    }
    for (const letter of d.right) {
      const b = make(letter);
      if (b) {
        right.appendChild(b);
      }
    }
    f.appId = null;
    f.state = "";
  };

  // A popup takes the keyboard while it is open, as Plasma's popups and KWin's menus do: then Escape reaches it (the
  // window server's key handler in display.js takes the keys aimed at the screen and stops them there). When it
  // closes, the keyboard goes back to the screen.
  const takeKeys = (popup) => {
    popup.tabIndex = -1;
    popup.focus({ preventScroll: true });
  };
  const giveKeysBack = (popup) => {
    if (popup && popup.contains(document.activeElement)) {
      const s = document.getElementById("screen");
      if (s) {
        s.focus({ preventScroll: true });
      }
    }
  };
  // The window menu (the M button): what this window server can do with a window.
  let menuEl = null;
  const closeMenu = () => {
    if (menuEl) {
      giveKeysBack(menuEl);
      menuEl.remove();
      menuEl = null;
    }
  };
  window.addEventListener("keydown", (event) => {
    if (menuEl && event.key === "Escape") {
      event.preventDefault();
      closeMenu();
    }
  }, true);
  window.addEventListener("pointerdown", (event) => {
    if (menuEl && !menuEl.contains(event.target)) {
      closeMenu();
    }
  }, true);
  // KWin's window menu (the M button, or a right click on the title bar): what this server can do with a window.
  // Keep above/below, shade, desktops: with a server that has the Plasma shell (PLASMA-PROTOCOL.md).
  const windowMenu = (f, id, api, button, at) => {
    closeMenu();
    const h = host();
    if (!h || !h.chromeRoot) {
      return;
    }
    menuEl = document.createElement("div");
    menuEl.className = "rv-pmenu";
    const flags = f.s ? f.s.flags : 0, sf = f.s ? f.s.shellFlags || 0 : 0;
    const max = flags & 4, full = flags & 8;
    const shell = api.shell();
    // KWin's window menu (kwin src/useractions.cpp, UserActionsMenu::init): Desktops (with more than one), Maximize,
    // Minimize, Keep Above Others, More Actions (Move, Resize, Keep Below Others, Fullscreen; Shade is the page's
    // own addition), a line, Close. A QMenu in the Breeze style (breeze kstyle/breezestyle.cpp, menuItemSizeFromContents
    // and drawMenuItemControl; breezemetrics.h): the menu has checkable items, so every item has the check box
    // column (CheckBox_Size 20), then MenuItem_TextLeftMargin 8, the 16 icon, MenuItem_ItemSpacing 2 + 1, the text,
    // and the 20 wide arrow column (MenuButton_IndicatorWidth); MenuItem_MarginWidth/Height 4 around.
    const item = (label, iconName, run, checked, parent = menuEl) => {
      const b = document.createElement("button");
      b.className = "rv-pmenu-item" + (checked ? " rv-checked" : "");
      b.tabIndex = -1;
      const box = document.createElement("span");
      box.className = "rv-pmenu-check" + (checked === undefined ? " rv-pmenu-nocheck" : "");
      if (checked) {
        box.innerHTML = "<svg viewBox='0 0 16 16' width='16' height='16'><path d='M4 8 L7 11 L12.5 5.5' fill='none' stroke='currentColor' stroke-width='2' stroke-linejoin='miter'/></svg>";
      }
      b.appendChild(box);
      b.appendChild(iconName ? iconImg([iconName], 16) : document.createElement("span"));
      const span = document.createElement("span");
      span.className = "rv-pmenu-text";
      span.textContent = label;
      b.appendChild(span);
      b.addEventListener("pointerdown", (event) => event.preventDefault());
      b.addEventListener("click", (event) => {
        event.stopPropagation();
        closeMenu();
        run();
      });
      parent.appendChild(b);
      return b;
    };
    const line = (parent = menuEl) => parent.appendChild(document.createElement("hr"));
    const submenu = (label, iconName) => {
      const sub = document.createElement("div");
      sub.className = "rv-pmenu-sub";
      const head = item(label, iconName, () => {}, undefined);
      head.classList.add("rv-pmenu-has-sub");
      head.addEventListener("click", (event) => event.stopPropagation(), true);
      head.appendChild(sub);
      return sub;
    };
    if (shell.version && shell.count > 1) {
      const sub = submenu("Desktops", "virtual-desktops");
      item("All Desktops", "", () => api.raw(15, id, 0), !!(f.s && f.s.desktop === 0), sub);
      line(sub);
      for (let d = 1; d <= shell.count; d++) {
        const name = (shell.names[d - 1] && shell.names[d - 1].name) || "Desktop " + d;
        item(d + " " + name, "", () => api.raw(15, id, d), !!(f.s && f.s.desktop === d), sub);
      }
    }
    item("Maximize", "window-maximize", () => api.order("maximize", id), !!max);
    item("Minimize", "window-minimize", () => api.order("minimize", id), undefined);
    if (shell.version) {
      item("Keep Above Others", "window-keep-above", () => api.raw(11, id, 2), !!(sf & 1));
    }
    const more = submenu("More Actions", "overflow-menu");
    item("Move", "transform-move", () => api.startMove(id, false), undefined, more);
    item("Resize", "transform-scale", () => api.startMove(id, true), undefined, more);
    if (shell.version) {
      item("Keep Below Others", "window-keep-below", () => api.raw(12, id, 2), !!(sf & 2), more);
    }
    item("Fullscreen", "view-fullscreen", () => api.order("fullscreen", id), !!full, more);
    if (shell.version) {
      item("Shade", "window-shade", () => api.raw(13, id, 2), !!(sf & 4), more);
    }
    line();
    item("Close", "window-close", () => api.order("close", id), undefined);
    const root = h.chromeRoot.getBoundingClientRect();
    const k = root.width / (h.screenSize()[0] || 1);
    let x, y;
    if (at) {
      x = (at[0] - root.left) / k;
      y = (at[1] - root.top) / k;
    } else {
      const r = button.getBoundingClientRect();
      x = (r.left - root.left) / k;
      y = (r.bottom - root.top) / k;
    }
    h.chromeRoot.appendChild(menuEl);
    const [W, H] = h.screenSize();
    menuEl.style.left = Math.round(Math.max(0, Math.min(W - menuEl.offsetWidth, x))) + "px";
    menuEl.style.top = Math.round(Math.max(0, Math.min(H - menuEl.offsetHeight, y))) + "px";
    takeKeys(menuEl);
  };

  // Place the decoration for the window's state; returns how far it reaches beyond the frame: [l, t, r, b].
  const layoutFrame = (f, s, o) => {
    if (!theme || !f.deco) {
      return [0, 0, 0, 0];
    }
    const d = theme.decoration;
    const active = o.active, maximized = !!(s.flags & 4);
    const g = s.geometry;
    const state = [active, maximized, g[2], g[3], o.titleH, o.border, o.appId, theme.serial, o.shellFlags, o.desktop].join(",");
    for (const b of f.buttons) {
      const on = b.kind === "keepabove" ? o.shellFlags & 1 : b.kind === "keepbelow" ? o.shellFlags & 2 : b.kind === "shade" ? o.shellFlags & 4
        : b.kind === "alldesktops" ? o.desktop === 0 : 0;
      b.el.classList.toggle("rv-checked", !!on);
    }
    f.el.classList.toggle("rv-max", maximized);
    if (f.appId !== o.appId) {
      f.appId = o.appId;
      for (const b of f.buttons) {
        if (b.kind === "menu") {
          b.el.textContent = "";
          b.el.appendChild(iconImg(appIcons(o.appId), 16));
        }
      }
    }
    for (const b of f.buttons) {
      if (b.kind === "maximize") {
        b.el.dataset.glyph = maximized ? "R" : "A";
      }
    }
    if (d.kind === "aurorae") {
      return layoutAurorae(f, s, o, d, active, maximized, state);
    }
    // Breeze: the visible box is the title bar over the window's geometry, with the configured border around
    // it (none by default; also none when maximized), rounded at the top, an outline, and a shadow.
    const tiled = !!(o.shellFlags & 64);	// (quick tiled: square corners, no shadow, as KWin draws tiled windows)
    const vb = maximized && !d.borderOnMaximized ? 0 : d.border;
    const sideB = d.noSides ? 0 : vb;
    const st = f.deco.style;
    const x = o.border - sideB, y = o.border, w = g[2] + 2 * sideB, h = o.titleH + g[3] + vb;
    f.visible = [x, y, w, h];
    if (f.state !== state) {
      f.state = state;
      st.left = x + "px";
      st.top = y + "px";
      st.width = w + "px";
      st.height = h + "px";
      st.borderWidth = "0 " + sideB + "px " + vb + "px " + sideB + "px";
      const r = maximized || tiled ? 0 : d.radius;
      st.borderRadius = r + "px " + r + "px " + (vb > 0 && !maximized ? r : 0) + "px " + (vb > 0 && !maximized ? r : 0) + "px";
      const shadows = [];
      if (d.outline && !maximized) {
        shadows.push("0 0 0 1px var(" + (active ? "--k-outline" : "--k-outline-i") + ")");
      }
      if (d.shadow && !maximized && !tiled) {
        const [off, r1, a1, r2, a2] = d.shadow;
        const k = d.shadowStrength * (active ? 1 : 0.5) * 0.5;
        shadows.push("0 " + off + "px " + r1 + "px rgba(0,0,0," + (a1 * k).toFixed(3) + ")");
        shadows.push("0 " + Math.round(off / 2) + "px " + r2 + "px rgba(0,0,0," + (a2 * k).toFixed(3) + ")");
      }
      st.boxShadow = shadows.join(",");
      f.el.classList.toggle("rv-inactive", !active);
      // The title bar: inside the visible box's top, the geometry's width.
      const ts = f.title.style;
      ts.borderRadius = r + "px " + r + "px 0 0";
      const pad = d.smallSpacing * 2;
      ts.paddingLeft = ts.paddingRight = pad + "px";
      ts.setProperty("--k-btn", d.button + "px");
      ts.setProperty("--k-btn-gap", d.smallSpacing + "px");
      ts.setProperty("--k-btn-top", Math.max(0, Math.round((o.titleH - d.button) / 2)) + "px");
      // (both groups as wide as the wider, so that a caption centred between them is centred on the bar)
      const count = (side) => [...side].filter((x) => f.buttons.some((b) => b.letter === x)).length;
      ts.setProperty("--k-side-w", Math.max(count(d.left), count(d.right)) * (d.button + d.smallSpacing) + "px");
      // The caption: centred over the whole bar (AlignCenterFullWidth), or in the room between the buttons.
      f.text.style.textAlign = /Left/.test(d.align) ? "left" : /Right/.test(d.align) ? "right" : "center";
      f.title.classList.toggle("rv-full-center", d.align === "AlignCenterFullWidth");
    }
    if (!d.shadow || maximized) {
      return [1, 1, 1, 1];
    }
    const [off, r1] = d.shadow;
    return [r1, Math.max(1, r1 - off), r1, r1 + off];
  };

  const layoutAurorae = (f, s, o, d, active, maximized, state) => {
    const L = d.layout, g = s.geometry;
    const pick = (k) => (maximized && L[k + "Maximized"] !== null && L[k + "Maximized"] !== undefined ? L[k + "Maximized"] : L[k]);
    // (borders as KWin's Aurorae has them: the theme's, held between a least and a most for the border size)
    const clamp = { Tiny: [1, 4], NoSides: [1, 4], Normal: [4, 6], Large: [6, 8], VeryLarge: [8, 12], Huge: [12, 20],
                    VeryHuge: [23, 30], Oversized: [36, 48] }[d.borderSize] || [0, 0];
    const cl = (v) => Math.max(clamp[0], Math.min(clamp[1], v));
    const none = maximized || d.borderSize === "None";
    const bl = none || d.borderSize === "NoSides" ? 0 : cl(L.BorderLeft);
    const br = none || d.borderSize === "NoSides" ? 0 : cl(L.BorderRight);
    const bb = none ? 0 : cl(L.BorderBottom);
    const pl = maximized ? 0 : L.PaddingLeft, pt = maximized ? 0 : L.PaddingTop, pr = maximized ? 0 : L.PaddingRight, pb = maximized ? 0 : L.PaddingBottom;
    const frame = d.frames[(maximized ? "decoration-maximized" : "decoration") + (active ? "" : "-inactive")] || d.frames.decoration;
    if (f.state !== state) {
      f.state = state;
      const st = f.deco.style;
      // The decoration's box: the window with its borders and title bar, and the theme's padding (its shadow) around.
      const x = o.border - bl - pl, y = o.border - pt;
      const w = g[2] + bl + br + pl + pr, h = o.titleH + g[3] + bb + pt + pb;
      f.visible = [o.border - bl, o.border, g[2] + bl + br, o.titleH + g[3] + bb];
      // (KWin draws the whole frame, its middle too, and the window over it: a theme's pieces may reach under the
      // window, or stop short of a title bar taller than they are; the window's own rectangle is cut out here)
      const cx = pl + bl, cy = pt + o.titleH;
      st.cssText = "left:" + x + "px;top:" + y + "px;width:" + w + "px;height:" + h + "px;box-sizing:border-box;" + frameCss(frame, true)
        + "clip-path:path(evenodd,'M0 0H" + w + "V" + h + "H0Z M" + cx + " " + cy + "h" + g[2] + "v" + g[3] + "h" + -g[2] + "Z');";
      const ts = f.title.style;
      const edgeTop = pick("TitleEdgeTop"), edgeLeft = pick("TitleEdgeLeft"), edgeRight = pick("TitleEdgeRight");
      ts.paddingLeft = Math.max(0, edgeLeft - bl) + "px";
      ts.paddingRight = Math.max(0, edgeRight - br) + "px";
      ts.borderRadius = "0";
      ts.setProperty("--k-btn", L.ButtonWidth + "px");
      ts.setProperty("--k-btn-h", L.ButtonHeight + "px");
      ts.setProperty("--k-btn-gap", L.ButtonSpacing + "px");
      ts.setProperty("--k-btn-top", (edgeTop + pick("ButtonMarginTop")) + "px");
      ts.setProperty("--k-caption-top", edgeTop + "px");
      ts.setProperty("--k-caption-h", Math.max(L.TitleHeight, L.ButtonHeight) + "px");
      ts.color = css(active ? d.activeText : d.inactiveText);
      f.text.style.textAlign = /center/i.test(d.align) ? "center" : /right/i.test(d.align) ? "right" : "left";
      f.text.style.margin = "0 " + L.TitleBorderRight + "px 0 " + L.TitleBorderLeft + "px";
      f.el.classList.toggle("rv-inactive", !active);
      for (const b of f.buttons) {
        auroraeButton(b, d, active, maximized);
      }
    }
    return [pl + bl + 1, pt + 1, pr + br + 1, pb + bb + 1];
  };
  const auroraeButton = (b, d, active, maximized) => {
    const name = b.kind === "maximize" && maximized && d.buttons.restore ? "restore" : b.kind;
    const states = d.buttons[name];
    const el = b.el;
    if (!states) {
      el.classList.add("rv-pbtn-fallback");	// (a theme without this button: Breeze's glyph)
      return;
    }
    el.classList.remove("rv-pbtn-fallback");
    const w = d.layout.widths[b.kind];
    el.style.width = w === undefined || w === d.layout.ButtonWidth ? "" : w + "px";
    const look = (s) => {
      const x = states[s];
      if (!x) {
        return "none";
      }
      return x.image ? "url(" + x.image.url + ")" : x.frame ? "url(" + x.frame.url + ")" : "none";
    };
    el.style.setProperty("--aur-normal", look(active ? "active" : "inactive"));
    el.style.setProperty("--aur-hover", look(active ? "hover" : "hover-inactive"));
    el.style.setProperty("--aur-pressed", look(active ? "pressed" : "pressed-inactive"));
    el.classList.add("rv-pbtn-aurorae");
  };

  // ---- the panel (display.js gives it its element, the windows and the programs whenever they changed): Plasma's
  // applets in the panel's order, those this page has: the launcher (Kickoff), the task manager (icons, with
  // the pinned launchers), the system tray (the keyboard layout), the digital clock, Peek at Desktop. The pager
  // shows the one desktop there is.

  const panel = {
    // The strip the page keeps for the panel (the server keeps windows off it): its thickness and floating gap.
    strip: () => (theme ? struts().bottom : 44),
    struts,
  };
  const el = (tag, cls, parent) => {
    const e = document.createElement(tag);
    if (cls) {
      e.className = cls;
    }
    if (parent) {
      parent.appendChild(e);
    }
    return e;
  };
  const pbutton = (cls, parent, title) => {
    const b = el("button", cls, parent);
    b.tabIndex = -1;
    if (title) {
      b.title = title;
    }
    b.addEventListener("pointerdown", (event) => event.preventDefault());
    return b;
  };
  const frameDiv = (f, cls, parent, fill = true) => {
    const d = el("div", cls, parent);
    if (f) {
      d.style.cssText = frameCss(f, fill);
    }
    return d;
  };

  let clockTimer = 0;
  const tickClock = () => {
    for (const c of document.querySelectorAll(".rv-pp-clock")) {
      const d = new Date();
      c.querySelector(".rv-pp-time").textContent = d.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
      c.querySelector(".rv-pp-date").textContent = d.toLocaleDateString([], { year: "2-digit", month: "numeric", day: "numeric" });
      c.title = d.toLocaleDateString([], { weekday: "long", year: "numeric", month: "long", day: "numeric" });
    }
  };

  // Windows grouped by program, the panel's launchers first (in their order), as Plasma's icon task manager has them.
  const taskGroups = (model, launchers) => {
    const groups = [];
    const byKey = new Map();
    for (const url of launchers) {
      const l = launcherApp(url);
      if (l && !byKey.has(l.app.file)) {
        const g = { key: l.app.file, launcher: l, app: l.app, windows: [] };
        byKey.set(g.key, g);
        groups.push(g);
      }
    }
    for (const w of model.windows.slice().sort((a, b) => a.id - b.id)) {
      if (w.shellFlags & 8) {
        continue;	// (skip taskbar: a window rule, or the program asked)
      }
      const app = appFor(w.app);
      const key = app ? app.file : "app:" + (w.app || w.id);
      let g = byKey.get(key);
      if (!g) {
        g = { key, launcher: null, app, appId: w.app, windows: [] };
        byKey.set(key, g);
        groups.push(g);
      }
      g.windows.push(w);
    }
    return groups;
  };

  // All the panels, in a layer of their own over the screen (display.js's #rv-panel: the screen's size, scaled with
  // it); each at its edge, floating (its gap all round) unless a maximized window touches it.
  const renderPanel = (root, model) => {
    if (!theme) {
      return;
    }
    root.classList.add("rv-plasma-panel");
    let layer = root.querySelector(":scope > .rv-pp-layer");
    if (!layer) {
      layer = el("div", "rv-pp-layer", null);
      root.insertBefore(layer, root.firstChild);
    }
    layer.textContent = "";
    for (const P of theme.panel.panels) {
      renderOnePanel(layer, P, model);
    }
    tickClock();
    if (!clockTimer) {
      clockTimer = setInterval(tickClock, 1000);
    }
  };
  const renderOnePanel = (layer, P, model) => {
    const T = theme.plasmaTheme;
    const [W, H] = model.screen;
    const floating = P.floating && !model.anyMaximized;
    const m = floating ? P.margin : 0;
    const thick = floating ? P.thickness : P.thickness + P.margin;
    // Panels on the sides lie between the top and bottom panels' strips, as in Plasma.
    const strip = struts();
    const lo = P.vertical ? strip.top : 0, hi = P.vertical ? H - strip.bottom : W;
    const along = hi - lo;
    let len = along - 2 * m;
    if (P.lengthMode === 2 && P.length > 0) {
      len = Math.min(len, P.length);
    }
    const place = (length) => {
      let start = lo + m;
      if (length < along - 2 * m) {
        start = P.align === "start" ? lo + m + P.offset : P.align === "end" ? hi - m - length - P.offset : lo + (along - length) / 2 + P.offset;
      }
      return start;
    };
    const start = place(len);
    const box = el("div", "rv-pp rv-pp-loc" + P.location + (floating ? " rv-pp-floating" : " rv-pp-flat") + (P.vertical ? " rv-pp-vertical" : "")
                   + (P.visibility === 1 ? " rv-pp-autohide" : ""), layer);
    const st = box.style;
    if (P.vertical) {
      st.top = start + "px";
      st.height = len + "px";
      st.width = thick + "px";
      st[P.location === 5 ? "left" : "right"] = m + "px";
    } else {
      st.left = start + "px";
      st.width = len + "px";
      st.height = thick + "px";
      st[P.location === 3 ? "top" : "bottom"] = m + "px";
    }
    if (floating && T.panelShadow) {
      const sh = frameDiv(T.panelShadow, "rv-pp-shadow", box, false);
      const [t, r, b, l] = T.panelShadow.slice;
      sh.style.inset = -t + "px " + -r + "px " + -b + "px " + -l + "px";
    }
    const edgeFrame = T.panelEdges && T.panelEdges[P.location];
    const bgFrame = floating ? T.panel : edgeFrame || T.panelFlat || T.panel;
    frameDiv(bgFrame, "rv-pp-bg", box);
    const row = el("div", "rv-pp-row", box);
    const margins = bgFrame ? bgFrame.margins : [4, 4, 4, 4];
    row.style.padding = margins[0] + "px " + margins[1] + "px " + margins[2] + "px " + margins[3] + "px";
    const inner = thick - (P.vertical ? margins[1] + margins[3] : margins[0] + margins[2]);
    row.style.setProperty("--pp-h", inner + "px");
    const kickoffIcon = theme.panel.kickoffIcon;
    if (P.lengthMode === 1) {
      // (fit the content: laid out once the applets are in, below)
      queueMicrotask(() => {
        const need = (P.vertical ? row.scrollHeight : row.scrollWidth) + 2;
        const fit = Math.min(along - 2 * m, Math.max(need, thick));
        if (P.vertical) {
          box.style.height = fit + "px";
          box.style.top = place(fit) + "px";
        } else {
          box.style.width = fit + "px";
          box.style.left = place(fit) + "px";
        }
      });
    }
    for (const plugin of P.applets) {
      if (/kickoff|kicker/.test(plugin)) {
        const b = pbutton("rv-pp-item rv-pp-kickoff rv-panel-start", row, "Application Launcher");
        b.appendChild(iconImg([kickoffIcon, "start-here-kde-symbolic", "start-here-kde", "start-here"], Math.round(inner * 0.8)));
        b.addEventListener("click", () => model.toggleLauncher());
      } else if (/pager/.test(plugin)) {
        // The virtual desktops in kwinrc's rows (the server's, PLASMA-PROTOCOL.md; one without them: one desktop).
        const sh = model.shell;
        const pager = el("div", "rv-pp-pager", row);
        const cols = Math.ceil(sh.count / sh.rows);
        pager.style.gridTemplateColumns = "repeat(" + cols + ", 1fr)";
        pager.style.gridTemplateRows = "repeat(" + sh.rows + ", 1fr)";
        pager.style.setProperty("--pager-cols", String(cols));
        pager.style.setProperty("--pager-rows", String(sh.rows));
        for (let d = 1; d <= sh.count; d++) {
          const name = (sh.names[d - 1] && sh.names[d - 1].name) || "Desktop " + d;
          const desk = pbutton("rv-pp-desk" + (d === sh.current ? " rv-active" : ""), pager, name);
          // (the windows on it, as small rectangles: their icons would not fit)
          for (const w of model.windows) {
            if (!w.minimized && (w.desktop === d || w.desktop === 0) && w.box) {
              const r = el("div", "rv-pp-desk-win", desk);
              const [W, H] = model.screen;
              r.style.cssText = "left:" + (100 * w.box[0] / W) + "%;top:" + (100 * w.box[1] / H) + "%;width:" + (100 * w.box[2] / W) + "%;height:"
                + (100 * w.box[3] / H) + "%";
            }
          }
          desk.addEventListener("click", () => {
            if (sh.version && d !== sh.current) {
              model.order(16, 0, d);	// (switch to that desktop)
            }
          });
        }
        pager.addEventListener("wheel", (event) => {
          if (sh.version && sh.count > 1) {
            model.order(21, 0, event.deltaY > 0 ? 5 : 6);	// (the next or previous desktop)
          }
        });
      } else if (/icontasks|taskmanager/.test(plugin)) {
        const labelled = /org\.kde\.plasma\.taskmanager$/.test(plugin) && !P.vertical;
        const tasks = el("div", "rv-pp-tasks" + (labelled ? " rv-pp-labelled" : ""), row);
        const iconSize = Math.max(16, Math.round(Math.min(48, inner * 0.72)));
        for (const g of taskGroups(model, P.launchers)) {
          const focused = g.windows.some((w) => w.id === model.focus && !w.minimized);
          const allMin = g.windows.length && g.windows.every((w) => w.minimized);
          const attention = !focused && g.windows.some((w) => w.shellFlags & 32);
          const state = !g.windows.length ? null : focused ? "focus" : attention ? "attention" : allMin ? "minimized" : "normal";
          const b = pbutton("rv-pp-item rv-pp-task" + (state ? " rv-" + state : " rv-launcher"), tasks,
                            g.windows.length ? g.windows.map((w) => w.title || w.app).join("\n") : (g.app ? g.app.name : g.key));
          b.dataset.windows = g.windows.map((w) => w.id).join(",");
          if (state && T.task[state]) {
            frameDiv(T.task[state], "rv-pp-task-bg", b);
          }
          if (T.task.hover) {
            frameDiv(T.task.hover, "rv-pp-task-hover", b);
          }
          b.appendChild(iconImg([g.app && g.app.icon, ...appIcons(g.appId || (g.windows[0] && g.windows[0].app))], iconSize));
          if (labelled) {
            // (the task manager with labels, org.kde.plasma.taskmanager: one button per window, its title beside the icon)
            el("span", "rv-pp-task-label", b).textContent = g.windows.length ? (g.windows[0].title || g.windows[0].app) : (g.app ? g.app.name : "");
          }
          if (g.windows.length > 1) {
            el("span", "rv-pp-count", b).textContent = String(g.windows.length);
          }
          b.addEventListener("click", () => {
            if (!g.windows.length) {
              if (g.launcher) {
                model.launch(g.launcher.index);
              }
              return;
            }
            if (g.windows.length === 1) {
              const w = g.windows[0];
              if (w.id === model.focus && !w.minimized) {
                model.minimize(w.id);
              } else {
                model.bring(w);
              }
              return;
            }
            // Several: the next one after the one in front.
            const at = g.windows.findIndex((w) => w.id === model.focus);
            model.bring(g.windows[(at + 1) % g.windows.length]);
          });
          b.addEventListener("auxclick", (event) => {
            if (event.button === 1 && g.launcher) {
              model.launch(g.launcher.index);	// (middle click: a new instance, as Plasma does)
            }
          });
        }
      } else if (/panelspacer/.test(plugin)) {
        el("div", "rv-pp-spacer", row);
      } else if (/marginsseparator/.test(plugin)) {
        el("div", "rv-pp-sep", row);
      } else if (/systemtray/.test(plugin)) {
        const tray = el("div", "rv-pp-tray", row);
        if (model.layout) {
          const kb = el("div", "rv-pp-item rv-pp-kbd", tray);
          kb.textContent = model.layout.replace(/-.*$/, "");
          kb.title = "Keyboard layout: " + model.layout;
        }
        // What a Plasma tray shows by default, as far as this machine has it: the clipboard, the sound volume, the
        // network (the browser's connection), notifications. Static but sane: they say what is true here.
        const trayIcon = (names, title, onClick) => {
          const b = pbutton("rv-pp-item rv-pp-tray-icon", tray, title);
          b.appendChild(iconImg(names, 22));
          if (onClick) {
            b.addEventListener("click", onClick);
          }
          return b;
        };
        for (const item of trayItems) {
          if (item.status === "Passive") {
            continue;
          }
          const b = pbutton("rv-pp-item rv-pp-tray-icon rv-pp-sni", tray, item.tooltip || item.title || item.id);
          if (item.iconPixmap && item.iconPixmap.data && item.iconPixmap.width <= 256 && item.iconPixmap.height <= 256) {
            const { width, height, data } = item.iconPixmap;
            const c = document.createElement("canvas");
            c.width = width;
            c.height = height;
            c.style.width = c.style.height = "22px";
            const img = c.getContext("2d").createImageData(width, height);
            for (let i = 0; i < width * height; i++) {
              img.data[4 * i] = data[4 * i + 1];	// (ARGB, big-endian)
              img.data[4 * i + 1] = data[4 * i + 2];
              img.data[4 * i + 2] = data[4 * i + 3];
              img.data[4 * i + 3] = data[4 * i];
            }
            c.getContext("2d").putImageData(img, 0, 0);
            b.appendChild(c);
          } else {
            b.appendChild(iconImg([item.iconName, "application-x-executable"], 22));
          }
          const send = (type, event) => {
            channel.postMessage({ type, key: item.key, x: Math.round(event.clientX), y: Math.round(event.clientY) });
            const h = host();
            if (h && h.ringBell) {
              h.ringBell();
            }
          };
          b.addEventListener("click", (event) => send("tray-activate", event));
          b.addEventListener("auxclick", (event) => {
            if (event.button === 1) {
              send("tray-secondary", event);
            }
          });
        }
        trayIcon(["klipper-symbolic", "edit-paste"], "Clipboard: shared with the browser (Ctrl+V on the screen pastes the host's)");
        trayIcon(["audio-volume-muted-symbolic", "audio-volume-muted"], "Audio: no sound device in this machine");
        const online = navigator.onLine !== false;
        trayIcon([online ? "network-wired-activated-symbolic" : "network-wired-disconnected-symbolic", online ? "network-wired" : "network-offline"],
                 online ? "Network: through the browser's connection" : "Network: the browser is offline");
        const bell = trayIcon(["notification-inactive-symbolic", "preferences-desktop-notification-bell", "notifications"], "Notifications: none",
                              (event) => toggleHistory(event.currentTarget));
        bell.classList.add("rv-pp-bell");
        queueMicrotask(updateBell);
        const exp = pbutton("rv-pp-item rv-pp-tray-icon", tray, "Show hidden icons");
        exp.appendChild(iconImg([P.location === 3 ? "arrow-down" : P.location === 5 ? "arrow-right" : P.location === 6 ? "arrow-left" : "arrow-up", "go-up"], 16));
      } else if (/digitalclock/.test(plugin)) {
        const c = pbutton("rv-pp-item rv-pp-clock", row, "");
        el("div", "rv-pp-time", c);
        el("div", "rv-pp-date", c);
        c.addEventListener("click", () => toggleCalendar(c));
      } else if (/showdesktop|minimizeall/.test(plugin)) {
        const b = pbutton("rv-pp-item rv-pp-peek" + (model.shell.showing ? " rv-checked" : ""), row, "Peek at Desktop");
        b.appendChild(iconImg(["user-desktop", "desktop"], Math.round(Math.min(22, inner * 0.6))));
        b.addEventListener("click", () => model.showDesktop());
      }
    }
  };

  // A popup of a panel (the launcher, the calendar) next to the applet it belongs to, on the screen's side of its
  // panel, as Plasma places them: above a bottom panel, below a top one, beside a side one; kept on the screen.
  const placePopup = (popup, w, h, selector) => {
    const layer = popup.parentNode;
    const button = layer && layer.querySelector(selector);
    const [W, H] = (host() && host().screenSize()) || [1024, 768];
    let x = 8, y = H - h - 8, loc = 4;
    if (button) {
      const lr = layer.getBoundingClientRect(), br = button.getBoundingClientRect();
      const k = lr.width / W || 1;
      const bx = (br.left - lr.left) / k, by = (br.top - lr.top) / k, bw = br.width / k, bh = br.height / k;
      const pbox = button.closest(".rv-pp");
      const m = /rv-pp-loc(\d)/.exec(pbox ? pbox.className : "");
      loc = m ? Number(m[1]) : 4;
      const pr = pbox ? pbox.getBoundingClientRect() : br;
      const px = (pr.left - lr.left) / k, py = (pr.top - lr.top) / k, pw = pr.width / k, ph = pr.height / k;
      const gap = 6;
      if (loc === 4) {
        x = bx;
        y = py - h - gap;
      } else if (loc === 3) {
        x = bx;
        y = py + ph + gap;
      } else if (loc === 5) {
        x = px + pw + gap;
        y = by;
      } else {
        x = px - w - gap;
        y = by;
      }
    }
    popup.style.left = Math.round(Math.max(4, Math.min(W - w - 4, x))) + "px";
    popup.style.top = Math.round(Math.max(4, Math.min(H - h - 4, y))) + "px";
    popup.dataset.edge = String(loc);
  };

  // ---- Overview (KWin 6's, Meta+W): the windows of this desktop side by side over the dimmed, blurred desktop, as
  // big as they fit; typing filters them by title; a click (or Enter on the one picked) brings one forward.
  const renderOverview = (root, items, thumbs, pick, close) => {
    root.textContent = "";
    root.className = "rv-overview";
    const search = el("input", "rv-ov-search", root);
    search.placeholder = "Type to search\u2026";
    search.spellcheck = false;
    const grid = el("div", "rv-ov-grid", root);
    let selected = 0;
    const draw = () => {
      grid.textContent = "";
      const want = search.value.trim().toLowerCase();
      const shown = items.filter((w) => !want || ((w.title || "") + " " + (w.app || "")).toLowerCase().includes(want));
      selected = Math.min(selected, Math.max(0, shown.length - 1));
      // (as KWin lays them out: about as many columns as rows, wider than tall, each at most a third of the screen)
      const n = Math.max(1, shown.length), cols = Math.max(1, Math.round(Math.sqrt(n * 1.5))), rows = Math.ceil(n / cols);
      const box = root.getBoundingClientRect(), k = box.width / ((host() && host().screenSize()[0]) || box.width || 1) || 1;
      const cw = Math.min(box.width / k / cols - 32, (box.width / k) / 2.2), ch = Math.min((box.height / k - 120) / rows - 40, (box.height / k) / 2.2);
      grid.style.gridTemplateColumns = "repeat(" + cols + ", " + Math.max(120, Math.floor(cw)) + "px)";
      grid.style.gridTemplateRows = "repeat(" + rows + ", " + Math.max(90, Math.floor(ch)) + "px)";
      shown.forEach((w, i) => {
        const cell = el("div", "rv-ov-item" + (i === selected ? " rv-selected" : ""), grid);
        cell.addEventListener("pointerenter", () => {
          for (const x of grid.children) {
            x.classList.toggle("rv-selected", x === cell);
          }
          selected = i;
        });
        const pic = el("div", "rv-ov-thumb", cell);
        const c = thumbs.get(w.id);
        if (c) {
          // (the window as KWin's Overview shows it: with its title bar, scaled whole into the cell)
          const win = el("div", "rv-ov-win", pic);
          el("div", "rv-ov-bar", win).textContent = w.title || w.app || "";
          win.appendChild(c);
          const tw = Math.max(120, Math.floor(cw)) - 16, th = Math.max(90, Math.floor(ch)) - 36 - 24;
          const f = Math.min(1, tw / c.width, th / c.height);	// (never bigger than the window is)
          c.style.width = Math.round(c.width * f) + "px";
          c.style.height = Math.round(c.height * f) + "px";
        } else {
          pic.appendChild(iconImg(appIcons(w.app), 96));
        }
        const cap = el("div", "rv-ov-caption", cell);
        cap.appendChild(iconImg(appIcons(w.app), 22));
        el("span", "", cap).textContent = w.title || w.app || "window " + w.id;
        cell.addEventListener("click", (event) => {
          event.stopPropagation();
          pick(w);
        });
      });
      if (!shown.length) {
        el("div", "rv-ov-none", grid).textContent = items.length ? "No matches" : "No windows";
      }
      return shown;
    };
    let shown = draw();
    search.addEventListener("input", () => {
      shown = draw();
    });
    search.addEventListener("keydown", (event) => {
      event.stopPropagation();
      if (event.key === "Escape") {
        close();
      } else if (event.key === "Enter") {
        if (shown[selected]) {
          pick(shown[selected]);
        }
      } else if (/^Arrow/.test(event.key)) {
        event.preventDefault();
        selected = Math.max(0, Math.min(shown.length - 1, selected + (/Right|Down/.test(event.key) ? 1 : -1)));
        shown = draw();
      }
    });
    root.addEventListener("click", () => close());
    search.focus({ preventScroll: true });
  };

  // ---- notifications (the server is org.freedesktop.Notifications: PLASMA-PROTOCOL.md section 3): popups in
  // Plasma's look in the corner of the screen nearest the tray, the newest at the bottom, a history behind the
  // tray's bell. The body keeps the spec's markup (b, i, u, a as text) and nothing else.
  const notes = new Map();	// id -> { el, timer, m }
  const history = [];
  let unread = 0;
  const tell = (m) => {
    channel.postMessage(m);
    const h = host();
    if (h && h.ringBell) {
      h.ringBell();
    }
  };
  const safeBody = (text) => {
    const box = document.createElement("div");
    const doc = new DOMParser().parseFromString("<div>" + String(text || "").replace(/<br\s*\/?>/gi, "\n") + "</div>", "text/html");
    const walk = (from, to) => {
      for (const n of from.childNodes) {
        if (n.nodeType === 3) {
          to.appendChild(document.createTextNode(n.textContent));
        } else if (n.nodeType === 1) {
          const tag = n.tagName.toLowerCase();
          const keep = ["b", "i", "u"].includes(tag) ? document.createElement(tag) : tag === "a" ? document.createElement("u") : null;
          walk(n, keep ? to.appendChild(keep) : to);
        }
      }
    };
    walk(doc.body.firstChild || doc.body, box);
    return box;
  };
  const notesRoot = () => {
    const panelRoot = document.getElementById("rv-panel");
    if (!panelRoot) {
      return null;
    }
    let r = panelRoot.querySelector(":scope > .rv-notes");
    if (!r) {
      r = el("div", "rv-notes", panelRoot);
    }
    // Where: the corner by the tray (bottom right for a bottom panel), inside the panels' strips, popupEdgeDistance
    // (gridUnit x 2) from them; as wide as popupWidth (gridUnit x 18) plus the dialog's margins (plasma-workspace
    // applets/notifications global/Globals.qml; the space between popups, popupSpacing, is a gridUnit: breeze.css).
    const st = struts();
    const pad = theme && theme.plasmaTheme.dialog ? theme.plasmaTheme.dialog.margins : [4, 4, 4, 4];
    r.style.width = (18 * 18 + pad[1] + pad[3]) + "px";
    const tray = panelRoot.querySelector(".rv-pp-tray");
    const pbox = tray && tray.closest(".rv-pp");
    const loc = pbox ? Number((/rv-pp-loc(\d)/.exec(pbox.className) || [0, 4])[1]) : 4;
    r.style.left = r.style.right = r.style.top = r.style.bottom = "";
    r.style[loc === 5 ? "left" : "right"] = ((loc === 5 ? st.left : st.right) + 36) + "px";
    r.style[loc === 3 ? "top" : "bottom"] = ((loc === 3 ? st.top : st.bottom) + 36) + "px";
    r.style.flexDirection = loc === 3 ? "column" : "column-reverse";
    return r;
  };
  const noteImage = (m, size) => {
    const img = m.image;
    if (img && img.data && img.width && img.height && img.width <= 512 && img.height <= 512) {
      const c = document.createElement("canvas");
      c.width = img.width;
      c.height = img.height;
      const out = c.getContext("2d").createImageData(img.width, img.height);
      const ch = img.channels || (img.hasAlpha ? 4 : 3);
      for (let y = 0; y < img.height; y++) {
        for (let x = 0; x < img.width; x++) {
          const i = y * img.rowstride + x * ch, o = (y * img.width + x) * 4;
          out.data[o] = img.data[i];
          out.data[o + 1] = img.data[i + 1];
          out.data[o + 2] = img.data[i + 2];
          out.data[o + 3] = ch === 4 ? img.data[i + 3] : 255;
        }
      }
      c.getContext("2d").putImageData(out, 0, 0);
      c.className = "rv-note-image";
      c.style.width = c.style.height = size + "px";
      return c;
    }
    const names = [m.imagePath, m.appIcon, m.desktopEntry, "preferences-desktop-notification-bell"].filter((x) => x && typeof x === "string");
    return iconImg(names, size, "rv-icon rv-note-image");
  };
  const notify = (m) => {
    if (!theme) {
      return;
    }
    const root = notesRoot();
    if (!root) {
      return;
    }
    const T = theme.plasmaTheme;
    let n = notes.get(m.id);
    if (n) {
      clearTimeout(n.timer);
      n.el.textContent = "";
    } else {
      n = { el: el("div", "rv-note" + (m.urgency === 2 ? " rv-note-critical" : ""), root) };
      notes.set(m.id, n);
    }
    n.m = m;
    if (T.dialogShadow) {
      const sh = frameDiv(T.dialogShadow, "rv-k-shadow", n.el, false);
      const [t, r, b, l] = T.dialogShadow.slice;
      sh.style.inset = -t + "px " + -r + "px " + -b + "px " + -l + "px";
    }
    frameDiv(T.dialog, "rv-k-bg", n.el);
    const body = el("div", "rv-note-body", n.el);
    const pad = T.dialog ? T.dialog.margins : [8, 8, 8, 8];
    body.style.padding = pad[0] + "px " + pad[1] + "px " + pad[2] + "px " + pad[3] + "px";
    const head = el("div", "rv-note-head", body);
    head.appendChild(iconImg([m.desktopEntry, m.appIcon && !m.appIcon.startsWith("/") ? m.appIcon : null, "preferences-desktop-notification-bell"].filter(Boolean), 16));
    el("span", "rv-note-app", head).textContent = m.appName || "Notification";
    el("span", "rv-note-time", head).textContent = new Date(m.time || Date.now()).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    const close = pbutton("rv-note-close", head, "Close");
    close.innerHTML = glyph("X");
    close.addEventListener("click", (event) => {
      event.stopPropagation();
      unnotify(m.id, 2, true);
    });
    const main = el("div", "rv-note-main", body);
    const text = el("div", "rv-note-text", main);
    el("div", "rv-note-summary", text).textContent = m.summary || "";
    if (m.body) {
      const b = safeBody(m.body);
      b.className = "rv-note-message";
      text.appendChild(b);
    }
    if (m.image || m.imagePath) {
      main.appendChild(noteImage(m, 48));
    }
    const actions = (m.actions || []).filter((a) => a[0] !== "default");
    if (actions.length) {
      const row = el("div", "rv-note-actions", body);
      for (const [key, label] of actions) {
        const b = pbutton("rv-note-action", row, label);
        b.textContent = label;
        b.addEventListener("click", (event) => {
          event.stopPropagation();
          tell({ type: "notify-action", id: m.id, action: key });
          if (!m.resident) {
            unnotify(m.id, 2, true);
          }
        });
      }
    }
    n.el.addEventListener("click", () => {
      if ((m.actions || []).some((a) => a[0] === "default")) {
        tell({ type: "notify-action", id: m.id, action: "default" });
      }
      unnotify(m.id, 2, true);
    });
    const timeout = m.timeout === 0 || m.urgency === 2 ? 0 : m.timeout > 0 ? m.timeout : 5000;
    if (timeout) {
      n.timer = setTimeout(() => unnotify(m.id, 1, true), timeout);
      n.el.addEventListener("pointerenter", () => clearTimeout(n.timer));
      n.el.addEventListener("pointerleave", () => {
        n.timer = setTimeout(() => unnotify(m.id, 1, true), 2000);
      });
    }
    if (!m.transient) {
      const at = history.findIndex((x) => x.id === m.id);
      if (at >= 0) {
        history.splice(at, 1);
      }
      history.unshift(m);
      history.length = Math.min(history.length, 50);
      unread++;
      updateBell();
    }
  };
  // A popup goes: expired (1), dismissed (2), or closed by its program (3; then the server is not told again).
  const unnotify = (id, reason, tellServer) => {
    const n = notes.get(id);
    if (n) {
      clearTimeout(n.timer);
      n.el.classList.add("rv-note-gone");
      setTimeout(() => n.el.remove(), 150 * Math.max(0.01, theme ? theme.anim : 1));
      notes.delete(id);
    }
    if (reason === 3) {
      const at = history.findIndex((x) => x.id === id);
      if (at >= 0 && history[at].resident) {
        history.splice(at, 1);
      }
    }
    if (tellServer) {
      tell({ type: "notify-closed", id, reason });
    }
  };
  const updateBell = () => {
    for (const b of document.querySelectorAll(".rv-pp-bell")) {
      b.classList.toggle("rv-unread", unread > 0);
      b.title = unread ? unread + " new notification" + (unread > 1 ? "s" : "") : history.length ? "Notifications" : "Notifications: none";
    }
  };
  // The history, from the tray's bell.
  let historyEl = null;
  const closeHistory = () => {
    if (historyEl) {
      giveKeysBack(historyEl);
      historyEl.remove();
      historyEl = null;
    }
  };
  window.addEventListener("keydown", (event) => {
    if (historyEl && event.key === "Escape") {
      closeHistory();
    }
  }, true);
  const toggleHistory = (bell) => {
    if (historyEl) {
      closeHistory();
      return;
    }
    const layer = document.getElementById("rv-panel");	// (not the panels' layer: it is drawn again when they change)
    if (!layer || !theme) {
      return;
    }
    unread = 0;
    updateBell();
    const T = theme.plasmaTheme;
    const pop = el("div", "rv-calendar rv-history", layer);
    historyEl = pop;
    const w = 340, h = Math.min(420, 90 + 64 * Math.max(1, history.length));
    pop.style.width = w + "px";
    pop.style.height = h + "px";
    frameDiv(T.dialog, "rv-k-bg", pop);
    const body = el("div", "rv-cal-body", pop);
    const pad = T.dialog ? T.dialog.margins : [8, 8, 8, 8];
    body.style.padding = pad[0] + "px " + pad[1] + "px " + pad[2] + "px " + pad[3] + "px";
    el("div", "rv-cal-day", body).textContent = "Notifications";
    const list = el("div", "rv-history-list", body);
    if (!history.length) {
      el("div", "rv-k-none", list).textContent = "No unread notifications";
    }
    for (const m of history) {
      const row = el("div", "rv-history-item", list);
      row.appendChild(iconImg([m.desktopEntry, m.appIcon && !m.appIcon.startsWith("/") ? m.appIcon : null, "preferences-desktop-notification-bell"].filter(Boolean), 22));
      const t = el("div", "rv-note-text", row);
      el("div", "rv-note-summary", t).textContent = (m.appName ? m.appName + ": " : "") + (m.summary || "");
      if (m.body) {
        const b = safeBody(m.body);
        b.className = "rv-note-message";
        t.appendChild(b);
      }
    }
    placePopup(pop, w, h, ".rv-pp-bell");
    takeKeys(pop);
  };
  window.addEventListener("pointerdown", (event) => {
    if (historyEl && !historyEl.contains(event.target) && !event.target.closest(".rv-pp-bell")) {
      closeHistory();
    }
  }, true);

  // PlasmaExtras.Highlight: the theme's widgets/viewitem "hover" frame over the whole item.
  const kickHighlightOf = (parent) => frameDiv(theme.plasmaTheme.item.hover, "rv-k-hl", parent);
  const padOf = (f, fallback) => (f ? f.margins : fallback).map((v) => v + "px").join(" ");
  // A PlasmaComponents ToolButton with an icon only: padded by the theme's widgets/button "toolbutton-hover" margins
  // around at least a gridUnit (18; ButtonBackground.qml), that frame drawn under it while the pointer is on it.
  const plasmaToolButton = (cls, parent, title, icons, size = 16) => {
    const T = theme.plasmaTheme;
    const b = pbutton("rv-ptool " + cls, parent, title);
    frameDiv(T.toolButton, "rv-ptool-hover", b);
    b.style.padding = padOf(T.toolButton, [6, 6, 6, 6]);
    b.appendChild(iconImg(icons, size));
    return b;
  };
  // A PlasmaComponents TabButton: padded by the theme's widgets/tabbar "north-active-tab" margins; the current one
  // with that frame under it.
  const plasmaTab = (cls, parent, text, current) => {
    const T = theme.plasmaTheme;
    const b = pbutton("rv-ptab " + cls + (current ? " rv-active" : ""), parent);
    if (current) {
      frameDiv(T.tab, "rv-ptab-bg", b);
    }
    b.style.padding = padOf(T.tab, [10, 6, 10, 6]);
    el("span", "", b).textContent = text;
    return b;
  };
  // ---- the digital clock's popup: the date and a month's calendar (Plasma's has more: holidays, time zones, events).
  let calendar = null;
  const closeCalendar = () => {
    if (calendar) {
      giveKeysBack(calendar.el);
      calendar.el.remove();
      calendar = null;
    }
  };
  window.addEventListener("pointerdown", (event) => {
    if (calendar && !calendar.el.contains(event.target) && !event.target.closest(".rv-pp-clock")) {
      closeCalendar();
    }
  }, true);
  window.addEventListener("keydown", (event) => {
    if (calendar && event.key === "Escape") {
      closeCalendar();
    }
  }, true);
  const toggleCalendar = (clockButton) => {
    if (calendar) {
      closeCalendar();
      return;
    }
    const layer = document.getElementById("rv-panel");
    if (!layer || !theme) {
      return;
    }
    const T = theme.plasmaTheme;
    const pop = el("div", "rv-calendar rv-clock-cal", layer);
    // The digital clock's popup without the agenda (plasma-workspace applets/digitalclock CalendarView.qml): at least
    // gridUnit x 22 by gridUnit x 25 (Kirigami's gridUnit: 18). The month view (components/calendar MonthView.qml,
    // MonthViewHeader.qml, DaysCalendar.qml, DayDelegate.qml): a heading (Kirigami.Heading level 1: the font at 1.35)
    // with the month, and the year when it is not this year's; the Days, Months, Years tabs and the previous, today and
    // next buttons; then the grid: the days of the week (at 0.4 opacity) over six weeks, the cells sharing the space,
    // each number at the larger of 1.35 x the font and a third of the cell; days of other months at 0.5; today
    // highlighted (PlasmaExtras.Highlight: the theme's viewitem hover frame), a day under the pointer at 0.3 of it.
    const w = 18 * 22, h = 18 * 25;
    pop.style.width = w + "px";
    pop.style.height = h + "px";
    if (T.dialogShadow) {
      const sh = frameDiv(T.dialogShadow, "rv-k-shadow", pop, false);
      const [t, r, b, l] = T.dialogShadow.slice;
      sh.style.inset = -t + "px " + -r + "px " + -b + "px " + -l + "px";
    }
    frameDiv(T.dialog, "rv-k-bg", pop);
    const body = el("div", "rv-cal-body", pop);
    const pad = T.dialog ? T.dialog.margins : [4, 4, 4, 4];
    body.style.padding = pad[0] + "px " + pad[1] + "px " + pad[2] + "px " + pad[3] + "px";
    calendar = { el: pop, month: new Date(), view: 0 };	// (view: 0 days, 1 months, 2 years)
    calendar.month.setDate(1);
    const toolButton = (parent, title, icons, run) => {
      const b = plasmaToolButton("rv-cal-nav", parent, title, icons);
      b.addEventListener("click", run);
      return b;
    };
    const render = () => {
      body.textContent = "";
      const today = new Date();
      const m = calendar.month;
      const head = el("div", "rv-cal-headbox", body);
      const title = el("div", "rv-cal-title", head);
      const monthName = m.toLocaleDateString([], { month: "long" });
      title.textContent = calendar.view === 0 && m.getFullYear() === today.getFullYear() ? monthName
        : calendar.view === 0 ? monthName + " " + m.getFullYear() : String(m.getFullYear());
      const row = el("div", "rv-cal-tabrow", head);
      const tabs = el("div", "rv-cal-tabs", row);
      ["Days", "Months", "Years"].forEach((name, i) => {
        const t = plasmaTab("rv-cal-tab", tabs, name, i === calendar.view);
        t.addEventListener("click", () => {
          calendar.view = i;
          render();
        });
      });
      const step = (k) => () => {
        if (calendar.view === 0) {
          m.setMonth(m.getMonth() + k);
        } else {
          m.setFullYear(m.getFullYear() + k * (calendar.view === 1 ? 1 : 10));
        }
        render();
      };
      const names = [["Previous Month", "Next Month"], ["Previous Year", "Next Year"], ["Previous Decade", "Next Decade"]][calendar.view];
      toolButton(row, names[0], ["go-previous"], step(-1));
      toolButton(row, "Today", ["go-jump-today"], () => {
        calendar.month = new Date();
        calendar.month.setDate(1);
        calendar.view = 0;
        render();
      });
      toolButton(row, names[1], ["go-next"], step(1));
      const grid = el("div", "rv-cal-grid rv-cal-view" + calendar.view, body);
      const cell = (text, cls, pick) => {
        const c = el("div", "rv-cal-cell" + (cls ? " " + cls : ""), grid);
        kickHighlightOf(c);
        el("span", "rv-cal-num", c).textContent = text;
        if (pick) {
          c.addEventListener("click", pick);
        }
        return c;
      };
      if (calendar.view === 0) {
        const weekStart = 1;	// (Monday, as Plasma's default for most locales)
        for (let i = 0; i < 7; i++) {
          const d = new Date(2024, 0, 1 + ((weekStart + i - 1) % 7));	// (2024-01-01 was a Monday)
          el("div", "rv-cal-wd", grid).textContent = d.toLocaleDateString([], { weekday: "short" });
        }
        const lead = (m.getDay() - weekStart + 7) % 7;
        const start = new Date(m);
        start.setDate(1 - lead);
        for (let i = 0; i < 42; i++) {
          const d = new Date(start);
          d.setDate(start.getDate() + i);
          cell(String(d.getDate()), (d.getMonth() !== m.getMonth() ? "rv-cal-other" : "") + (d.toDateString() === today.toDateString() ? " rv-cal-today" : ""));
        }
      } else if (calendar.view === 1) {
        for (let i = 0; i < 12; i++) {
          const d = new Date(m.getFullYear(), i, 1);
          cell(d.toLocaleDateString([], { month: "short" }), m.getFullYear() === today.getFullYear() && i === today.getMonth() ? "rv-cal-today" : "", () => {
            m.setMonth(i);
            calendar.view = 0;
            render();
          });
        }
      } else {
        const first = Math.floor(m.getFullYear() / 10) * 10 - 1;
        for (let i = 0; i < 12; i++) {
          const y = first + i;
          cell(String(y), (i === 0 || i === 11 ? "rv-cal-other" : "") + (y === today.getFullYear() ? " rv-cal-today" : ""), () => {
            m.setFullYear(y);
            calendar.view = 1;
            render();
          });
        }
      }
      // (the numbers: the larger of a level 1 heading and a third of the cell; the days of the week: the larger of
      // the small font and a third of the cell)
      const rows = calendar.view === 0 ? 7 : 4;
      const cellH = grid.clientHeight / rows;
      grid.style.setProperty("--cal-num", Math.max(theme.font.px * 1.35, cellH / (calendar.view === 0 ? 3 : 6)) + "px");
      grid.style.setProperty("--cal-wd", Math.max(theme.small.px, cellH / 3) + "px");
    };
    render();
    placePopup(pop, w, h, ".rv-pp-clock");
    takeKeys(pop);
  };

  // ---- the launcher: Kickoff's layout (a header with the user and the search field, the categories at the side,
  // the programs in a list; Favorites as a grid), drawn on the Plasma theme's dialog background.
  const CATEGORIES = [
    ["Development", ["Development"], "applications-development"], ["Education", ["Education"], "applications-education"],
    ["Games", ["Game"], "applications-games"], ["Graphics", ["Graphics"], "applications-graphics"],
    ["Internet", ["Network"], "applications-internet"], ["Multimedia", ["AudioVideo", "Audio", "Video"], "applications-multimedia"],
    ["Office", ["Office"], "applications-office"], ["Science & Math", ["Science"], "applications-science"],
    ["Settings", ["Settings"], "preferences-system"], ["System", ["System"], "applications-system"],
    ["Utilities", ["Utility"], "applications-utilities"],
  ];
  // Kickoff's metrics, as plasma-desktop's applets/kickoff/package/contents/ui has them (KickoffListDelegate.qml,
  // KickoffGridDelegate.qml, KickoffSingleton.qml, AbstractKickoffItemDelegate.qml, Footer.qml), in Kirigami's units
  // (kirigami src/platform/units.cpp: smallSpacing 4, mediumSpacing 6, largeSpacing 8; icon sizes small 16,
  // smallMedium 22, medium 32, large 48) and the font's own metrics: a Label is as high as the font's ascent plus
  // descent (QFontMetrics::height, which is what CSS's line-height "normal" gives as well), and the delegates'
  // spacing is the font's descent.
  //   list row: smallSpacing above and below; left: the Plasma theme's widgets/listitem "normal" margin; right: that
  //     margin plus the descent; a medium (32) icon, then (twice the listitem margin further) the name over the
  //     description in the small font, no space between; a row without a description is as high as one with.
  //   category row: the same paddings, a smallMedium (22) icon, the name 1.5 lines high.
  //   grid cell: square, 2 x smallSpacing above and below, a large (48) icon, the descent, the name in two lines.
  // Kickoff opens four cells wide and four high (main.qml minimumGridRowCount), the categories at the side as wide as
  // the widest of them.
  const U = { small: 4, medium: 6, large: 8, iconSmall: 16, iconSmallMedium: 22, iconMedium: 32, iconLarge: 48 };
  let measureCtx = null;
  const fontBox = (font) => {
    measureCtx = measureCtx || document.createElement("canvas").getContext("2d");
    measureCtx.font = font;
    const m = measureCtx.measureText("Xg");
    return { ascent: m.fontBoundingBoxAscent, descent: m.fontBoundingBoxDescent, height: m.fontBoundingBoxAscent + m.fontBoundingBoxDescent };
  };

  // (a line's height as the page lays it out: CSS's "normal" line height, the font's ascent plus descent)
  const lineHeight = (parent, font) => {
    const probe = el("span", "", parent);
    probe.style.cssText = "position: absolute; visibility: hidden; display: inline-block; line-height: normal; white-space: nowrap; font: " + font;
    probe.textContent = "Xg";
    const h = probe.offsetHeight;
    probe.remove();
    return h;
  };
  const kickMetrics = (root) => {
    const li = theme.plasmaTheme.listItem ? theme.plasmaTheme.listItem.margins : [6, 6, 6, 6];	// (Breeze's: 6)
    const f = { height: lineHeight(root, fontCss(theme.font)), descent: fontBox(fontCss(theme.font)).descent };
    const sm = { height: lineHeight(root, fontCss(theme.small)) };
    const d = f.descent > 0 && f.descent < f.height / 2 ? f.descent : Math.round(f.height * 0.215);	// (Noto Sans: 0.293 of 1.362)
    return {
      li, d, text: f.height, small: sm.height,
      row: U.small * 2 + Math.max(U.iconMedium, f.height + sm.height),
      catRow: U.small * 2 + Math.max(U.iconSmallMedium, Math.round(f.height * 1.5)),
      cell: Math.ceil(U.small * 2 * 2 + U.iconLarge + d + f.height * 2),
    };
  };
  const px = (v) => (Math.round(v * 100) / 100) + "px";
  // The highlight of the current entry: PlasmaExtras.Highlight, the Plasma theme's widgets/viewitem "hover" frame
  // over the whole delegate (icon, name and description).
  const kickHighlight = kickHighlightOf;
  let kick = null;	// { root, model, filter, list, category, selected }
  const openLauncher = (root, model) => {
    if (!theme) {
      return;
    }
    const T = theme.plasmaTheme;
    root.textContent = "";
    root.className = "rv-kickoff";
    const M = kickMetrics(root);
    const pad = T.dialog ? T.dialog.margins : [4, 4, 4, 4];
    const vars = {
      "--k-li-t": px(M.li[0]), "--k-li-r": px(M.li[1]), "--k-li-b": px(M.li[2]), "--k-li-l": px(M.li[3]), "--k-descent": px(M.d),
      "--k-text-h": px(M.text), "--k-small-h": px(M.small), "--k-row": px(M.row), "--k-cat-row": px(M.catRow), "--k-cell": px(M.cell),
      "--k-pad-t": px(pad[0]), "--k-pad-r": px(pad[1]), "--k-pad-b": px(pad[2]), "--k-pad-l": px(pad[3]),
    };
    for (const [name, value] of Object.entries(vars)) {
      root.style.setProperty(name, value);
    }
    const [W, H] = model.screen;
    if (T.dialogShadow) {
      const sh = frameDiv(T.dialogShadow, "rv-k-shadow", root, false);
      const [t, r, b, l] = T.dialogShadow.slice;
      sh.style.inset = -t + "px " + -r + "px " + -b + "px " + -l + "px";
    }
    frameDiv(T.dialog, "rv-k-bg", root);
    const body = el("div", "rv-k-body", root);
    body.style.padding = pad[0] + "px " + pad[1] + "px " + pad[2] + "px " + pad[3] + "px";
    const head = el("div", "rv-k-head", body);
    const who = el("div", "rv-k-who", head);
    who.appendChild(iconImg(["user-identity", "avatar-default"], U.iconMedium, "rv-icon rv-k-avatar"));
    el("span", "rv-k-user", who).textContent = "root";
    const field = el("div", "rv-k-field", head);
    field.appendChild(iconImg(["search", "edit-find"], U.iconSmall, "rv-icon rv-k-field-icon"));
    const filter = el("input", "rv-k-search", field);
    filter.placeholder = "Search…";
    filter.spellcheck = false;
    const main = el("div", "rv-k-main", body);
    const side = el("div", "rv-k-side", main);
    const list = el("div", "rv-k-list", main);
    const foot = el("div", "rv-k-foot", body);
    const tab = el("div", "rv-k-tab rv-active", foot);
    tab.appendChild(iconImg(["applications-all-symbolic", "view-list-icons", "applications-other"], U.iconSmallMedium));
    el("span", "", tab).textContent = "Applications";
    kick = { root, model, filter, list, side, category: "Favorites", selected: 0 };
    const cats = [["Favorites", null, "bookmarks"], ["All Applications", null, "applications-all-symbolic"]];
    const used = new Set(model.apps.flatMap((a) => (a.categories || "").split(";")));
    for (const c of CATEGORIES) {
      if (c[1].some((x) => used.has(x))) {
        cats.push(c);
      }
    }
    for (const [name, , iconName] of cats) {
      const b = pbutton("rv-k-cat" + (name === kick.category ? " rv-active" : ""), side);
      kickHighlight(b);
      b.appendChild(iconImg([iconName, "applications-other"], U.iconSmallMedium));
      el("span", "rv-k-name", b).textContent = name;
      b.addEventListener("click", () => {
        kick.category = name;
        filter.value = "";
        for (const x of side.children) {
          x.classList.toggle("rv-active", x === b);
        }
        renderKickList();
      });
    }
    // The size: the categories as wide as the widest (its icon, twice the listitem margin, the name, the margins and
    // the descent), four grid cells beside them, the header and footer as they come out.
    // (the names' widths as laid out; again once the fonts have come, if they are still on their way)
    const size = () => {
      side.style.width = "max-content";
      const text = Math.max(...[...side.querySelectorAll(".rv-k-name")].map((n) => n.scrollWidth));
      const catW = Math.ceil(text + M.li[3] * 3 + U.iconSmallMedium + M.li[1] + M.d);
      side.style.width = px(catW + pad[1]);
      root.style.setProperty("--k-cats-w", px(catW + pad[1]));
      const w = Math.min(Math.ceil(pad[3] + catW + pad[1] + 1 + pad[3] + 4 * M.cell + pad[1]), W - 16);
      root.style.width = w + "px";
      const fixed = pad[0] + head.offsetHeight + foot.offsetHeight + pad[2];	// (the paddings, the header and the footer)
      const h = Math.min(Math.ceil(fixed + M.li[0] + 4 * M.cell + M.li[2]), H - 16);
      root.style.height = h + "px";
      placePopup(root, w, h, ".rv-pp-kickoff");
    };
    size();
    if (document.fonts && document.fonts.status === "loading") {
      document.fonts.ready.then(() => {
        if (kick && kick.root === root && root.isConnected && root.style.display !== "none" && !kick.filter.value) {
          openLauncher(root, model);
        }
      });
    }
    filter.addEventListener("input", renderKickList);
    filter.addEventListener("keydown", (event) => {
      event.stopPropagation();
      const items = [...list.querySelectorAll(".rv-k-app")];
      if (event.key === "Escape") {
        model.toggleLauncher(false);
      } else if (event.key === "Enter") {
        const pick = items[kick.selected] || items[0];
        if (pick) {
          pick.click();
        }
      } else if (event.key === "ArrowDown" || event.key === "ArrowUp" || event.key === "ArrowRight" || event.key === "ArrowLeft") {
        event.preventDefault();
        selectKick(Math.max(0, Math.min(items.length - 1, kick.selected + (/Down|Right/.test(event.key) ? 1 : -1))), true);
      }
    });
    renderKickList();
    filter.focus({ preventScroll: true });
  };
  // One current entry, as a ListView has: the keys move it, and so does the pointer (Kickoff's delegates make the
  // entry under the pointer current), so there is never a second highlight.
  const selectKick = (i, scroll) => {
    const items = [...kick.list.querySelectorAll(".rv-k-app")];
    kick.selected = i;
    items.forEach((x, j) => x.classList.toggle("rv-selected", j === i));
    if (scroll && items[i]) {
      items[i].scrollIntoView({ block: "nearest" });
    }
  };
  const renderKickList = () => {
    if (!kick) {
      return;
    }
    const { model, filter, list } = kick;
    const want = filter.value.trim().toLowerCase();
    list.textContent = "";
    kick.selected = -1;
    let entries;
    let grid = false;
    if (want) {
      entries = model.apps.map((a, i) => [a, i]).filter(([a]) => (a.name + " " + a.comment + " " + a.categories + " " + a.file).toLowerCase().includes(want));
    } else if (kick.category === "Favorites") {
      entries = theme.panel.favorites.map((u) => launcherApp(u)).filter(Boolean).map((l) => [l.app, l.index]);
      grid = true;
    } else if (kick.category === "All Applications") {
      entries = model.apps.map((a, i) => [a, i]);
    } else {
      const c = CATEGORIES.find((x) => x[0] === kick.category);
      entries = model.apps.map((a, i) => [a, i]).filter(([a]) => c && c[1].some((x) => (a.categories || "").split(";").includes(x)));
    }
    list.classList.toggle("rv-k-grid", grid);
    entries.forEach(([a, i], n) => {
      const described = !grid && a.comment && a.comment !== a.name;
      const b = pbutton("rv-k-app" + (described ? "" : " rv-k-solo"), list, a.comment || a.name);
      kickHighlight(b);
      b.appendChild(iconImg([a.icon, "application-x-executable"], grid ? U.iconLarge : U.iconMedium));
      const label = el("span", "rv-k-name", b);
      label.textContent = a.name;
      if (described) {
        el("span", "rv-k-comment", b).textContent = a.comment;
      }
      b.addEventListener("click", () => model.launch(i));
      b.addEventListener("pointerenter", () => selectKick(n, false));
    });
    if (entries.length && want) {
      selectKick(0, false);
    }
    if (!entries.length) {
      el("div", "rv-k-none", list).textContent = model.apps.length ? "No matches" : "No applications found";
    }
  };

  // ---- the window switcher: KWin's Thumbnail Grid (Plasma 6's default; kwin src/plugins/windowswitchers/
  // thumbnail_grid main.qml), on the Plasma theme's dialog background, in the middle of the screen. Each cell: the
  // theme's widgets/viewitem "hover" margins around a gridUnit x 16 (288) wide thumbnail, as high as the screen's
  // shape makes it, over a caption row of gridUnit x 2; the content twice those margins in, a gridUnit between the
  // thumbnail and the caption; the window's icon at the huge size (64), centred, its middle a quarter of the icon
  // above the thumbnail's bottom; the caption centred, elided, bold on the current window. Cells touch; the current
  // one has the viewitem hover frame. As many columns as fit in 0.9 of the screen's width, about square.
  const renderSwitcher = (root, items, at, thumbs) => {
    if (!theme) {
      return;
    }
    const T = theme.plasmaTheme;
    root.textContent = "";
    root.classList.add("rv-plasma-switcher");
    if (T.dialogShadow) {
      const sh = frameDiv(T.dialogShadow, "rv-k-shadow", root, false);
      const [t, r, b, l] = T.dialogShadow.slice;
      sh.style.inset = -t + "px " + -r + "px " + -b + "px " + -l + "px";
    }
    frameDiv(T.dialog, "rv-k-bg", root);
    const grid = el("div", "rv-sw-grid", root);
    const pad = T.dialog ? T.dialog.margins : [4, 4, 4, 4];
    grid.style.padding = pad[0] + "px " + pad[1] + "px " + pad[2] + "px " + pad[3] + "px";
    const [W, H] = (host() && host().screenSize()) || [1280, 800];
    const m = T.item.hover ? T.item.hover.margins : [4, 4, 4, 4];
    const thumbW = 18 * 16, thumbH = Math.round(thumbW * H / W), caption = 18 * 2;
    const cellW = m[3] + thumbW + m[1], cellH = m[0] + caption + thumbH + m[2];
    const fit = Math.max(1, Math.floor(W * 0.9 / cellW));
    let cols = Math.min(items.length, fit);
    while (cols > 1 && (cols - 1) * (cols - 1) >= items.length) {
      cols--;	// (about square: the fewest columns that still make no more rows than columns)
    }
    grid.style.gridTemplateColumns = "repeat(" + cols + ", " + cellW + "px)";
    grid.style.gridAutoRows = cellH + "px";
    grid.style.maxHeight = Math.round(H * 0.7) + "px";
    grid.style.setProperty("--sw-in", m.map((v) => v * 2 + "px").join(" "));
    items.forEach((w, i) => {
      const cell = el("div", "rv-sw-item" + (i === at ? " rv-selected" : "") + (w.minimized ? " rv-sw-min" : ""), grid);
      if (i === at) {
        frameDiv(T.item.hover, "rv-sw-sel", cell);
      }
      const inner = el("div", "rv-sw-inner", cell);
      const pic = el("div", "rv-sw-thumb", inner);
      const c = thumbs.get(w.id);
      if (c) {
        pic.appendChild(c);
      }
      pic.appendChild(iconImg(appIcons(w.app), 64, "rv-icon rv-sw-icon"));
      el("div", "rv-sw-caption", inner).textContent = w.title || w.app || "window " + w.id;
    });
  };

  const ready = () => !!theme;
  // Where a window's task button is, in screen pixels ([x, y, w, h]; the minimize animation goes there), or null.
  const taskButtonRect = (id) => {
    const h = host();
    const layer = document.querySelector("#rv-panel .rv-pp-layer");
    if (!layer || !h) {
      return null;
    }
    const b = [...layer.querySelectorAll(".rv-pp-task")].find((x) => (x.dataset.windows || "").split(",").includes(String(id)));
    if (!b) {
      return null;
    }
    const lr = layer.getBoundingClientRect(), br = b.getBoundingClientRect();
    const k = lr.width / (h.screenSize()[0] || 1) || 1;
    return [(br.left - lr.left) / k, (br.top - lr.top) / k, br.width / k, br.height / k];
  };
  // What a frame was made for: the theme, and whether the server has the Plasma shell (its buttons).
  const frameKey = (shellVersion) => (theme ? theme.serial : 0) + ":" + (shellVersion ? 1 : 0);

  window.rvPlasma = {
    ready, ask, icon, iconImg, appIcons, appFor, launcherApp, cursor, wallpaper, decorateFrame, layoutFrame, panel,
    frameCss, closeMenu, renderPanel, openLauncher, renderSwitcher, log, frameKey, windowMenu, taskButtonRect, renderOverview,
    get theme() {
      return theme;
    },
    get serial() {
      return theme ? theme.serial : 0;
    },
    onChange: (f) => listeners.push(f),
    reload: () => {
      lastJson = "";
      fileCache.clear();
      listCache.clear();
      frameCache.clear();
      ask({ type: "config" });
    },
    // (for tests: the parsers)
    parseConfig, schemeColors,
  };
})();
