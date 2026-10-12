// Plasma's splash screen for the page's desktop: what KSplash shows while
// Plasma starts, from the moment the window server starts until the page has
// the Plasma desktop ready (the theme built, the wallpaper decoded, the panel
// drawn), then faded out. Without it the page would show its plain desktop
// (display.js's own background and panel) for the seconds plasma.js needs to
// read the guest's configuration and build the theme.
//
// How Plasma 6 does it, and what is taken from it (plasma-workspace 6.3:
// ksplash/ksplashqml/splashapp.cpp, splashwindow.cpp, startkde/startplasma.cpp,
// startkde/plasma-session/startup.cpp, shell/shellcorona.cpp; KWin 6.3's
// "login" effect, effects/login/contents/code/main.js):
//   - startplasma starts ksplashqml when ksplashrc [KSplash] Engine is
//     KSplashQML (the default); System Settings' "None" writes Engine=none
//     and Theme=None, and then nothing is shown. Theme is a Look-and-Feel
//     package; its contents/splash/Splash.qml is drawn on every screen, else
//     the Global Theme's (kdeglobals [KDE] LookAndFeelPackage), else Breeze's
//     (org.kde.breeze.desktop: Breeze Dark and Twilight have no splash of
//     their own and fall back to it).
//   - ksplashqml has the D-Bus service org.kde.KSplash, /KSplash setStage(s).
//     It counts the distinct stage names it hears ("initial" itself, and on
//     Wayland "wm" itself; "startPlasma" from startplasma, "kcminit",
//     "ksmserver", "ready" from plasma-session, "desktop" from plasmashell)
//     and hands the count to the QML as `stage`; at seven it quits.
//   - Breeze's Splash.qml: a black Rectangle; the Plasma logo
//     (images/plasma.svgz, Kirigami gridUnit * 8 square) in the middle; the
//     busy indicator (images/busywidget.svgz, gridUnit * 2, turning once in
//     2 s, still when animations are off) halfway between the logo's top and
//     the bottom; "Plasma made by KDE" (#eff0f1) and the KDE logo
//     (images/kde.svgz, gridUnit * 2) at the bottom right, gridUnit from the
//     edges, largeSpacing apart. All of it fades in (veryLongDuration * 2,
//     InOutQuad) at stage 2; the busy indicator fades out at stage 5.
//   - When ksplashqml quits, KWin's login effect fades its window out over
//     a second (animationTime(1000): scaled by AnimationDurationFactor).
//
// Here: the window server tells the page its stages (wlserver/boot.js:
// "initial" when its process starts, "ksmserver" when the session's services
// are on the bus; its table's announcement is "wm"), and it is org.kde.KSplash
// on the session bus, so a program that calls setStage advances this splash
// too (wlserver/ksplash.js). The page's own work makes the rest:
// "startPlasma" the configuration files read, "kcminit" the settings applied
// (the theme built: colours, fonts, cursors, icons), "desktop" the wallpaper
// decoded and the panel drawn, "ready" the desktop shown under the splash.
// The count drives the splash as in ksplashqml; the splash goes when the
// page's desktop is ready, whatever programs say. The line under the busy
// indicator says what is being waited for, in plain words.
//
// The splash drawn is the configured package's own Splash.qml, read for what
// a splash made from Breeze's has (themes such as Catppuccin's are): the root
// Rectangle's colour, a background Image filling it, the logo Image in the
// middle, a busy indicator Image (turned by a RotationAnimator), and the Row
// of texts and images at the bottom right, their sizes in Kirigami units. A
// splash that is something else (its own layout, shaders, particles) cannot
// be drawn here: Breeze's is drawn instead, from Breeze's package in the guest.
// QML is not run: its text is read, and only what is listed above is taken.
//
// Loaded by display.js with plasma.js (not with ?chrome=plain). Exposed as
// window.rvSplash: state() (for tests: shown, stages, text, theme, why).
(() => {
  "use strict";

  const BREEZE = "org.kde.breeze.desktop";
  const ORDER = [	// what is waited for, in the order the page does it, and how the line says it
    ["wm", "Starting window manager"],
    ["startPlasma", "Reading settings"],
    ["kcminit", "Applying settings"],
    ["ksmserver", "Starting session services"],
    ["desktop", "Loading desktop"],
    ["ready", "Loading desktop"],
  ];
  const STUCK_MS = 90000;		// no stage for this long: the splash goes anyway (the plain desktop is better than none)
  const WALLPAPER_MS = 20000;	// a wallpaper that cannot be decoded in this long is not waited for

  const log = [];
  const note = (line) => {
    console.log(line);
    log.push(line);
    if (log.length > 40) {
      log.shift();
    }
  };

  // ---- the splash's package, read from its Splash.qml

  // QML as a tree of objects: { type, on, id, props: { name: text }, children: [] }. Comments taken out (strings
  // kept), imports and pragmas skipped; property declarations' values kept by name; functions, signals and
  // handlers' blocks skipped. Enough for the declarative part of a splash; anything it does not know it passes by.
  const parseQml = (src) => {
    let s = "";
    for (let i = 0; i < src.length;) {
      const c = src[i];
      if (c === "\"" || c === "'" || c === "`") {
        let j = i + 1;
        while (j < src.length && src[j] !== c) {
          j += src[j] === "\\" ? 2 : 1;
        }
        s += src.slice(i, j + 1);
        i = j + 1;
      } else if (src.startsWith("//", i)) {
        while (i < src.length && src[i] !== "\n") {
          i++;
        }
      } else if (src.startsWith("/*", i)) {
        const j = src.indexOf("*/", i + 2);
        const end = j < 0 ? src.length : j + 2;
        s += src.slice(i, end).replace(/[^\n]/g, " ");
        i = end;
      } else {
        s += c;
        i++;
      }
    }
    let at = 0;
    const space = (lines = true) => {
      while (at < s.length && (lines ? /[\s;]/ : /[ \t\r]/).test(s[at])) {
        at++;
      }
    };
    const word = () => {
      const m = /^[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*/.exec(s.slice(at, at + 200));
      if (!m) {
        return null;
      }
      at += m[0].length;
      return m[0];
    };
    // An expression or a block, to its end: a newline, ';' or '}' outside brackets and strings.
    const value = () => {
      const start = at;
      let depth = 0;
      while (at < s.length) {
        const c = s[at];
        if (c === "\"" || c === "'" || c === "`") {
          at++;
          while (at < s.length && s[at] !== c) {
            at += s[at] === "\\" ? 2 : 1;
          }
          at++;
          continue;
        }
        if (c === "(" || c === "[" || c === "{") {
          depth++;
        } else if (c === ")" || c === "]" || c === "}") {
          if (depth === 0) {
            break;
          }
          depth--;
        } else if (depth === 0 && (c === "\n" || c === ";")) {
          break;
        }
        at++;
      }
      return s.slice(start, at).trim();
    };
    const object = (type) => {
      const o = { type, on: null, id: null, props: {}, children: [] };
      space(false);
      if (s.startsWith("on ", at) || s.startsWith("on\t", at)) {
        at += 2;
        space(false);
        o.on = word();
        space();
      }
      if (s[at] !== "{") {
        return null;
      }
      at++;
      for (let guard = 0; guard < 10000; guard++) {
        space();
        if (at >= s.length) {
          return o;
        }
        if (s[at] === "}") {
          at++;
          return o;
        }
        const before = at;
        let w = word();
        if (!w) {
          at++;	// (something not understood: passed by)
          continue;
        }
        if (w === "function" || w === "signal" || w === "enum" || w === "component" || w === "import") {
          value();
          continue;
        }
        if (/^(readonly|default|required)$/.test(w)) {
          space(false);
          w = word();
        }
        if (w === "property") {
          space(false);
          word();	// (the type; list<T> is not taken apart, nor needed)
          if (s[at] === "<") {
            while (at < s.length && s[at] !== ">") {
              at++;
            }
            at++;
          }
          space(false);
          const name = word();
          space(false);
          if (s[at] === ":") {
            at++;
            space(false);
            const v = /^[A-Z]/.test(s[at] || "") ? childOrValue() : value();
            if (name && typeof v === "string") {
              o.props[name] = v;
            }
          } else {
            value();
          }
          continue;
        }
        space(false);
        if (s[at] === ":") {
          at++;
          space(false);
          if (w === "id") {
            o.id = word();
            continue;
          }
          const v = /^[A-Z]/.test(s[at] || "") ? childOrValue() : value();
          if (typeof v === "string") {
            o.props[w] = v;
          } else if (v) {
            o.props[w] = v;	// (a grouped object value: kept as an object)
          }
          continue;
        }
        if (/^[A-Z]/.test(w.split(".").pop()) && (s[at] === "{" || s.startsWith("on", at))) {
          const child = object(w.split(".").pop());
          if (child) {
            o.children.push(child);
            continue;
          }
        }
        if (s[at] === "{") {
          at = before;	// (a grouped property "anchors { ... }": its contents as anchors.x)
          word();
          space(false);
          const group = object("");
          if (group) {
            for (const [k, v] of Object.entries(group.props)) {
              o.props[w + "." + k] = v;
            }
          }
          continue;
        }
        value();
      }
      return o;
    };
    // After "name:", an object ("Gradient { ... }") or an expression that starts with a capital (Kirigami.Units...).
    const childOrValue = () => {
      const save = at;
      const w = word();
      space(false);
      if (w && s[at] === "{") {
        return object(w.split(".").pop());
      }
      at = save;
      return value();
    };
    // The file: imports and pragmas, then one object.
    for (let guard = 0; guard < 1000 && at < s.length; guard++) {
      space();
      const w = word();
      if (!w) {
        return null;
      }
      if (w === "import" || w === "pragma") {
        value();
        continue;
      }
      return object(w.split(".").pop());
    }
    return null;
  };

  // A QML string: a literal, or the text of an i18n call (its last string), or null.
  const qmlString = (text) => {
    if (typeof text !== "string") {
      return null;
    }
    const t = text.trim();
    let m = /^"((?:[^"\\]|\\.)*)"$/.exec(t) || /^'((?:[^'\\]|\\.)*)'$/.exec(t);
    if (m) {
      return m[1].replace(/\\(.)/g, "$1");
    }
    if (/^(i18n|i18nc|i18nd|i18ndc|qsTr|qsTranslate)\s*\(/.test(t)) {
      const all = [...t.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1]);
      return all.length ? all[all.length - 1].replace(/\\(.)/g, "$1") : null;
    }
    return null;
  };
  // A number from an expression of numbers, Kirigami's units and the object's own properties, with + - * / and
  // brackets; null for anything else (nothing is evaluated as code).
  const qmlNumber = (text, units, own, depth = 0) => {
    if (typeof text !== "string" || depth > 4) {
      return null;
    }
    const names = /(?:Kirigami\.|PlasmaCore\.|PlasmaCore\.Theme\.)?Units\.(gridUnit|smallSpacing|largeSpacing|mediumSpacing|longDuration|shortDuration|veryLongDuration|veryShortDuration|iconSizes\.\w+)|\b([a-z_]\w*)\b/g;
    let bad = false;
    const expr = text.replace(names, (m, unit, local) => {
      if (unit) {
        const v = unit.startsWith("iconSizes.") ? units.icon[unit.slice(10)] : units[unit];
        if (v === undefined) {
          bad = true;
        }
        return String(v);
      }
      if (own && own[local] !== undefined) {
        const v = qmlNumber(own[local], units, own, depth + 1);
        if (v === null) {
          bad = true;
        }
        return "(" + v + ")";
      }
      bad = true;
      return m;
    });
    if (bad || !/^[\d.\s+\-*/()]+$/.test(expr)) {
      return null;
    }
    // (a small calculator: expressions of + - * / and brackets)
    let i = 0;
    const tok = expr.replace(/\s+/g, "");
    const num = () => {
      if (tok[i] === "(") {
        i++;
        const v = sum();
        i++;
        return v;
      }
      if (tok[i] === "-") {
        i++;
        return -num();
      }
      const m = /^\d*\.?\d+/.exec(tok.slice(i));
      if (!m) {
        throw new Error("number");
      }
      i += m[0].length;
      return Number(m[0]);
    };
    const product = () => {
      let v = num();
      while (tok[i] === "*" || tok[i] === "/") {
        const op = tok[i++];
        const r = num();
        v = op === "*" ? v * r : v / r;
      }
      return v;
    };
    const sum = () => {
      let v = product();
      while (tok[i] === "+" || tok[i] === "-") {
        const op = tok[i++];
        const r = product();
        v = op === "+" ? v + r : v - r;
      }
      return v;
    };
    try {
      const v = sum();
      return i === tok.length && isFinite(v) ? v : null;
    } catch (error) {
      return null;
    }
  };

  const all = (o, out = []) => {
    for (const c of o.children) {
      out.push(c);
      all(c, out);
    }
    return out;
  };
  const isImage = (o) => o.type === "Image" || o.type === "AnimatedImage";
  // An image's file in the package: a plain relative path ("images/plasma.svgz"), nothing else.
  const source = (o) => {
    const p = qmlString(o.props.source);
    return p && !p.includes(":") && !p.startsWith("/") && !p.split("/").includes("..") ? p : null;
  };
  // An item's size: sourceSize, else width/height, else what is given.
  const sizeOf = (o, units, fallback) => {
    const n = (k) => qmlNumber(o.props[k], units, o.props);
    const w = n("sourceSize.width") || n("width") || n("implicitWidth") || fallback;
    const h = n("sourceSize.height") || n("height") || n("implicitHeight") || fallback;
    return [w, h];
  };

  // What of a Splash.qml can be drawn: { color, background, logo, busy, row, fadeIn } or null.
  const readSplash = (src, units) => {
    const root = src ? parseQml(src) : null;
    if (!root || !/^(Rectangle|Item|Image)$/.test(root.type)) {
      return null;
    }
    const items = all(root);
    const model = { color: qmlString(root.props.color) || "#000000", background: null, logo: null, busy: null, row: null, fadeIn: 2 * units.veryLongDuration };
    const bg = root.type === "Image" ? root : items.find((o) => isImage(o) && /^parent$/.test(o.props["anchors.fill"] || ""));
    if (bg && source(bg)) {
      model.background = { src: source(bg), fill: /PreserveAspectCrop/.test(bg.props.fillMode || "") ? "cover"
                           : /PreserveAspectFit/.test(bg.props.fillMode || "") ? "contain" : /Tile/.test(bg.props.fillMode || "") ? "tile" : "fill" };
    }
    const logo = items.find((o) => isImage(o) && o !== bg && o.id === "logo")
      || items.find((o) => isImage(o) && o !== bg && /^parent$/.test(o.props["anchors.centerIn"] || ""));
    if (logo && source(logo)) {
      model.logo = { src: source(logo), size: sizeOf(logo, units, units.gridUnit * 8) };
    }
    const spinner = (o) => o.children.find((c) => c.type === "RotationAnimator" || c.type === "RotationAnimation"
                                                   || (c.type === "NumberAnimation" && /rotation/.test(c.on || c.props.property || "")));
    const busy = items.find((o) => isImage(o) && o !== logo && o !== bg && (/busy/i.test(o.id || "") || spinner(o)));
    if (busy && source(busy)) {
      const spin = spinner(busy);
      model.busy = { src: source(busy), size: sizeOf(busy, units, units.gridUnit * 2),
                     turn: spin ? qmlNumber(spin.props.duration, units, spin.props) || 2000 : 0 };
    }
    const row = items.find((o) => (o.type === "Row" || o.type === "RowLayout") && /bottom/.test(o.props["anchors.bottom"] || ""));
    if (row) {
      const parts = row.children.filter((c) => c.type === "Text" || c.type === "Label" || isImage(c)).map((c) => (c.type === "Text" || c.type === "Label"
        ? { text: qmlString(c.props.text), color: qmlString(c.props.color) }
        : { src: source(c), size: sizeOf(c, units, units.gridUnit * 2) })).filter((p) => p.text || p.src);
      if (parts.length) {
        const margin = qmlNumber(row.props["anchors.margins"], units, row.props);
        model.row = { parts, spacing: qmlNumber(row.props.spacing, units, row.props) || 0, margin: margin === null ? units.gridUnit : margin,
                      right: /right/.test(row.props["anchors.right"] || ""), left: /left/.test(row.props["anchors.left"] || "") };
      }
    }
    const intro = items.concat([root]).find((o) => o.id === "introAnimation") || root.children.find((c) => c.type === "OpacityAnimator");
    if (intro) {
      const d = qmlNumber(intro.props.duration, units, intro.props);
      if (d !== null) {
        model.fadeIn = d;
      }
    }
    if (!model.logo && !model.background) {
      return null;
    }
    return model;
  };

  // ---- files from the guest (the window server's file service, which plasma.js uses too)

  const fetchData = async (path) => {
    const P = window.rvPlasma;
    if (!P || !P.ask) {
      return null;
    }
    const r = await P.ask({ type: "fetch", path }, 10000);
    let data = r && r.data ? r.data : null;
    if (data && data[0] === 0x1f && data[1] === 0x8b) {
      try {
        const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream("gzip"));
        data = new Uint8Array(await new Response(stream).arrayBuffer());
      } catch (error) {
        data = null;
      }
    }
    return data;
  };
  const listDir = async (path) => {
    const P = window.rvPlasma;
    const r = P && P.ask ? await P.ask({ type: "list", path }, 10000) : null;
    return r && r.entries ? r.entries : null;
  };
  const urls = [];
  const imageUrl = async (path) => {
    const data = await fetchData(path);
    if (!data) {
      return null;
    }
    const ext = (/\.(\w+)$/.exec(path) || [, ""])[1].toLowerCase();
    const type = { svg: "image/svg+xml", svgz: "image/svg+xml", png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg",
                   gif: "image/gif", webp: "image/webp" }[ext];
    if (!type) {
      return null;
    }
    const url = URL.createObjectURL(new Blob([data], { type }));
    urls.push(url);
    return url;
  };

  // KConfig, as plasma.js reads it.
  const configOf = (files, home, name) => {
    const P = window.rvPlasma;
    const groups = new Map();
    for (const dir of ["/etc/xdg", home + "/.config/kdedefaults", home + "/.config"]) {
      for (const [g, keys] of P.parseConfig(files[dir + "/" + name] || "", home)) {
        groups.set(g, Object.assign(groups.get(g) || {}, keys));
      }
    }
    return (group, key, fallback) => {
      const g = groups.get(group);
      return g && g[key] !== undefined && g[key] !== "" ? g[key] : fallback;
    };
  };

  // Kirigami's units for the configured font (Kirigami 6: gridUnit is the font's line height, made even; the
  // spacings are fixed; durations scaled by kdeglobals [KDE] AnimationDurationFactor, as Plasma's Kirigami does).
  const unitsFor = (fontText, factor) => {
    const parts = String(fontText || "Noto Sans,10,-1,5,400").split(",");
    const px = Number(parts[2]) > 0 ? Number(parts[2]) : (Number(parts[1]) || 10) * 96 / 72;
    let gridUnit = Math.round(1.362 * px);
    gridUnit += gridUnit % 2;
    return { gridUnit, smallSpacing: 4, mediumSpacing: 6, largeSpacing: 8, veryShortDuration: 50 * factor, shortDuration: 100 * factor,
             longDuration: 200 * factor, veryLongDuration: 400 * factor, fontPx: px, family: parts[0] || "Noto Sans",
             icon: { small: 16, smallMedium: 22, medium: 32, large: 48, huge: 64, enormous: 128 } };
  };

  // Which splash, from the configuration: { none: true }, or { dir, model, units, factor, name }.
  const resolve = async (files, home) => {
    const ksplash = configOf(files, home, "ksplashrc");
    const globals = configOf(files, home, "kdeglobals");
    const reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    const factor = reduce ? 0 : Math.max(0, Math.min(4, Number(globals("KDE", "AnimationDurationFactor", "1")) || 0));
    const engine = ksplash("KSplash", "Engine", "KSplashQML");
    const theme = ksplash("KSplash", "Theme", "Breeze");
    if (engine !== "KSplashQML" || theme === "None") {
      return { none: true, factor, why: "ksplashrc: Engine " + engine + ", Theme " + theme };
    }
    const units = unitsFor(globals("General", "font"), factor);
    const lnf = globals("KDE", "LookAndFeelPackage", BREEZE);
    for (const name of [theme, lnf, BREEZE]) {
      if (!name || name.includes("/") || name === "." || name === "..") {
        continue;
      }
      for (const base of [home + "/.local/share/plasma/look-and-feel", "/usr/share/plasma/look-and-feel"]) {
        const dir = base + "/" + name + "/contents/splash";
        if (!(await listDir(dir))) {
          continue;
        }
        const data = await fetchData(dir + "/Splash.qml");
        const model = data ? readSplash(new TextDecoder("utf-8").decode(data), units) : null;
        if (model) {
          return { dir, model, units, factor, name };
        }
        note("splash: " + dir + "/Splash.qml cannot be drawn here; the next one is tried (Breeze's last)");
      }
    }
    return { none: false, model: null, units, factor, name: null, why: "no splash that can be drawn" };
  };

  // ---- the splash on the page

  let el = null, content = null, busyEl = null, line = null, ticking = false;
  let session = null;	// { pid, stages: Set, shownAt, lastStage, splash, done, wallpaperSince }
  let finished = 0;	// the pid whose splash has been and gone
  const style = () => {
    if (document.getElementById("rv-splash-style")) {
      return;
    }
    const s = document.createElement("style");
    s.id = "rv-splash-style";
    s.textContent = `
#rv-splash { position: absolute; inset: 0; z-index: 30000; overflow: hidden; background: #000; cursor: none;
             opacity: 1; transition-property: opacity; transition-timing-function: linear; }
#rv-splash .rv-splash-content { position: absolute; inset: 0; opacity: 0; transition-property: opacity;
             transition-timing-function: cubic-bezier(0.455, 0.03, 0.515, 0.955); }
#rv-splash img { position: absolute; display: block; user-select: none; -webkit-user-drag: none; }
#rv-splash .rv-splash-row { position: absolute; display: flex; align-items: center; white-space: nowrap; }
#rv-splash .rv-splash-row img { position: static; }
#rv-splash .rv-splash-busy { transition: opacity 800ms cubic-bezier(0.455, 0.03, 0.515, 0.955); }
#rv-splash .rv-splash-busy.rv-turning { animation: rv-splash-turn 2s linear infinite; }
#rv-splash .rv-splash-line { position: absolute; left: 0; right: 0; top: calc(75% - 9px); text-align: center; color: #eff0f1; opacity: 0;
             font: 400 13.33px "Noto Sans", sans-serif;
             transition: opacity 400ms ease; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
@keyframes rv-splash-turn { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }`;
    document.head.appendChild(s);
  };

  // Show the cover: black (Breeze's colour, and what Plasma shows before ksplash draws), the line under it.
  const begin = (pid, why) => {
    const host = window.rvDisplayHost;
    if (!host || !host.stage) {
      return;
    }
    end(true);
    style();
    session = { pid, stages: new Set(), shownAt: performance.now(), lastStage: performance.now(), splash: null, done: false,
                wallpaperSince: 0, resolving: false };
    el = document.createElement("div");
    el.id = "rv-splash";
    content = document.createElement("div");
    content.className = "rv-splash-content";
    el.appendChild(content);
    line = document.createElement("div");
    line.className = "rv-splash-line";
    el.appendChild(line);
    el.addEventListener("pointerdown", (event) => event.preventDefault());
    host.stage.appendChild(el);
    note("splash: shown (" + why + ", window server pid " + pid + ")");
    stage("initial");
    if (!ticking) {
      again();
    }
  };

  // Draw the package's splash into the cover (laid out for the cover's size now; again when it changes).
  const draw = async () => {
    const sp = session.splash;
    const m = sp.model, u = sp.units;
    el.style.background = m.color;
    const parts = [];
    if (m.background) {
      parts.push(imageUrl(sp.dir + "/" + m.background.src).then((url) => ["background", url]));
    }
    if (m.logo) {
      parts.push(imageUrl(sp.dir + "/" + m.logo.src).then((url) => ["logo", url]));
    }
    if (m.busy) {
      parts.push(imageUrl(sp.dir + "/" + m.busy.src).then((url) => ["busy", url]));
    }
    const rowUrls = m.row ? await Promise.all(m.row.parts.map((p) => (p.src ? imageUrl(sp.dir + "/" + p.src) : null))) : [];
    const got = new Map(await Promise.all(parts));
    if (!session || session.splash !== sp) {
      return;
    }
    const font = "\"" + u.family.replace(/"/g, "") + "\", \"Noto Sans\", sans-serif";
    if (m.background && got.get("background")) {
      const img = document.createElement("img");
      img.src = got.get("background");
      img.style.cssText = "inset:0;width:100%;height:100%;object-fit:" + ({ cover: "cover", contain: "contain" }[m.background.fill] || "fill");
      content.appendChild(img);
    }
    if (m.logo && got.get("logo")) {
      const img = document.createElement("img");
      img.className = "rv-splash-logo";
      img.src = got.get("logo");
      content.appendChild(img);
    }
    if (m.busy && got.get("busy")) {
      busyEl = document.createElement("img");
      busyEl.className = "rv-splash-busy" + (m.busy.turn && sp.factor > 0 ? " rv-turning" : "");
      busyEl.src = got.get("busy");
      if (m.busy.turn) {
        busyEl.style.animationDuration = m.busy.turn + "ms";	// (not scaled: Breeze's comment says why)
      }
      content.appendChild(busyEl);
    }
    if (m.row) {
      const row = document.createElement("div");
      row.className = "rv-splash-row";
      row.style.gap = m.row.spacing + "px";
      row.style.bottom = m.row.margin + "px";
      row.style[m.row.left && !m.row.right ? "left" : "right"] = m.row.margin + "px";
      m.row.parts.forEach((p, i) => {
        if (p.text) {
          const t = document.createElement("span");
          t.textContent = p.text;
          t.style.cssText = "color:" + (p.color || "#eff0f1") + ";font:400 " + u.fontPx.toFixed(2) + "px " + font;
          row.appendChild(t);
        } else if (rowUrls[i]) {
          const img = document.createElement("img");
          img.src = rowUrls[i];
          img.style.width = p.size[0] + "px";
          img.style.height = p.size[1] + "px";
          row.appendChild(img);
        }
      });
      content.appendChild(row);
      const text = m.row.parts.find((p) => p.text);
      if (text && text.color) {
        line.style.color = text.color;
      }
    }
    line.style.font = "400 " + u.fontPx.toFixed(2) + "px " + font;
    content.style.transitionDuration = Math.round(m.fadeIn) + "ms";
    laidFor = "";
    layout();
    // (faded in only once the pictures are decoded: a picture that arrives later would pop in, not fade)
    await Promise.all([...content.querySelectorAll("img")].map((img) => img.decode().catch(() => {})));
    sp.drawn = true;
    sync();
  };

  // Places from the cover's size, as the QML's anchors put them.
  let laidFor = "";
  const layout = () => {
    if (!el || !session || !session.splash || !session.splash.model) {
      return;
    }
    const W = el.clientWidth, H = el.clientHeight;
    const key = W + "x" + H;
    if (key === laidFor && content.childElementCount) {
      return;
    }
    laidFor = key;
    const m = session.splash.model, u = session.splash.units;
    const logo = content.querySelector(".rv-splash-logo");
    const [lw, lh] = m.logo ? m.logo.size : [0, 0];
    const logoY = Math.round((H - lh) / 2);
    if (logo) {
      logo.style.cssText = "left:" + Math.round((W - lw) / 2) + "px;top:" + logoY + "px;width:" + lw + "px;height:" + lh + "px";
    }
    let below = m.logo ? logoY + lh : Math.round(H / 2);
    if (busyEl) {
      const [bw, bh] = m.busy.size;
      // "in the middle of the remaining space": y = height - (height - logo.y) / 2 - height / 2
      const y = Math.round(H - (H - (m.logo ? logoY : H / 2)) / 2 - bh / 2);
      busyEl.style.left = Math.round((W - bw) / 2) + "px";
      busyEl.style.top = y + "px";
      busyEl.style.width = bw + "px";
      busyEl.style.height = bh + "px";
      below = y + bh;
    }
    line.style.top = Math.round(below + u.gridUnit / 2) + "px";
  };

  const stage = (name) => {
    if (!session || session.done || session.stages.has(name)) {
      return;
    }
    session.stages.add(name);
    session.lastStage = performance.now();
    note("splash: stage " + session.stages.size + " " + name + " at " + Math.round(performance.now() - session.shownAt) + " ms");
    sync();
  };

  // The splash as the count says (Breeze's onStageChanged), and the line.
  const sync = () => {
    if (!session || !el) {
      return;
    }
    const n = session.stages.size;
    const sp = session.splash;
    if (sp && sp.model && sp.drawn && n >= 2) {
      content.style.opacity = "1";
    }
    if (busyEl && n >= 5) {
      busyEl.style.opacity = "0";
    }
    const waiting = ORDER.find(([name]) => !session.stages.has(name));
    line.textContent = waiting ? waiting[1] + "…" : "";
    // (No line with no splash: "None" shows nothing at all.)
    line.style.opacity = sp && sp.none ? "0" : waiting ? "0.6" : "0";
  };

  // The page's own stages, looked at every frame while the splash is up.
  // (One loop at a time: `ticking` is true while a frame is asked for.)
  const again = () => {
    ticking = true;
    requestAnimationFrame(tick);
  };
  const tick = () => {
    ticking = false;
    if (!session || !el || session.done) {
      return;
    }
    const P = window.rvPlasma, D = window.rvDisplay;
    const now = performance.now();
    if (D && D.active() && !session.stages.has("wm")) {
      stage("wm");
    }
    // (The theme is the one built from this session's files: a theme from before, of a server that has gone, does
    // not count until the files have come; plasma.js builds nothing new when they are the same.)
    // (plasmashell as the desktop's shell, wlserver/plasmashell.js: the desktop is ready when its desktop, a layer
    // surface of the background layer, is on the screen)
    if (D && D.nativeShell && D.nativeShell() && D.active()) {
      if (D.windows().some((w) => w.kind === 6 && w.layer === 0)) {
        if (!session.stages.has("desktop")) {
          stage("desktop");
        } else if (!session.stages.has("ready")) {
          stage("ready");
        }
      }
    } else if (P && P.ready() && session.stages.has("startPlasma")) {
      stage("kcminit");
      const T = P.theme;
      let wallpaper = !T.wallpaper;
      if (!wallpaper && D) {
        const [w, h, s] = D.screenSize();
        const wp = P.wallpaper(w * s, h * s);
        wallpaper = !!(wp && wp.bitmap);
        session.wallpaperSince = session.wallpaperSince || now;
        if (!wallpaper && now - session.wallpaperSince > WALLPAPER_MS) {
          note("splash: the wallpaper is not decoded after " + WALLPAPER_MS / 1000 + " s; not waited for");
          wallpaper = true;
        }
      }
      const panel = !!document.querySelector("#rv-panel .rv-pp-layer");
      if (wallpaper && panel && D && D.active()) {
        if (!session.stages.has("desktop")) {
          stage("desktop");
        } else if (!session.stages.has("ready")) {
          stage("ready");	// (a frame after: the display has drawn the desktop under us)
        }
      }
    }
    if (session.stages.has("ready")) {
      if (!session.stages.has("ksmserver") && now - session.lastStage < 3000) {
        again();	// (the session's services, a moment more: the server says so soon after)
        return;
      }
      finish("the desktop is ready");
      return;
    }
    if (now - session.lastStage > STUCK_MS) {
      finish("no progress for " + STUCK_MS / 1000 + " s");
      return;
    }
    if (D && session.stages.has("wm") && !D.active() && now - session.shownAt > 5000 && session.wasActive) {
      end(false, "the window server has gone");
      return;
    }
    if (D && D.active()) {
      session.wasActive = true;
    }
    layout();
    again();
  };

  // Seven stages and out, faded as KWin's login effect fades ksplash's window.
  const finish = (why) => {
    if (!session || session.done) {
      return;
    }
    for (const [name] of ORDER) {
      session.stages.add(name);
    }
    session.done = true;
    sync();
    const factor = session.splash ? session.splash.factor : 1;
    const ms = Math.round(1000 * factor);
    note("splash: " + why + " after " + Math.round(performance.now() - session.shownAt) + " ms; fading out over " + ms + " ms");
    finished = session.pid;
    const gone = el;
    if (ms <= 0) {
      end(true);
      return;
    }
    gone.style.transitionDuration = ms + "ms";
    gone.style.pointerEvents = "none";
    requestAnimationFrame(() => {
      gone.style.opacity = "0";
    });
    setTimeout(() => {
      if (el === gone) {
        end(true);
      }
    }, ms + 50);
  };

  const end = (quiet, why) => {
    if (el) {
      el.remove();
    }
    if (!quiet && session) {
      note("splash: " + why);
      finished = session.pid;
    }
    el = content = busyEl = line = null;
    laidFor = "";
    session = null;
    for (const url of urls.splice(0)) {
      URL.revokeObjectURL(url);
    }
  };

  // The configuration (the server's answer to plasma.js, or sent unasked): which splash, once per session.
  const configured = async (m) => {
    if (!session || session.splash || session.resolving || !window.rvPlasma) {
      return;
    }
    session.resolving = true;
    stage("startPlasma");	// (the session's configuration files are read)
    const mine = session;
    let sp = null;
    try {
      sp = await resolve(m.files || {}, m.home || "/root");
    } catch (error) {
      note("splash: " + (error && error.stack || error));
    }
    if (session !== mine) {
      return;
    }
    if (!sp || (!sp.none && !sp.model)) {
      // (Breeze's design with nothing from the guest: the black cover and the line)
      sp = { none: false, model: null, units: unitsFor(null, 1), factor: sp ? sp.factor : 1, why: "nothing to draw" };
    }
    session.splash = sp;
    note("splash: " + (sp.none ? "none (" + sp.why + ")" : sp.model ? sp.name + " (" + sp.dir + ")" : "the cover only (" + sp.why + ")"));
    if (sp.model) {
      await draw();
    } else {
      sync();
    }
  };

  const display = window.rvChannel("rv-display");
  display.onmessage = (event) => {
    const m = event.data;
    if (!m || typeof m !== "object" || !m.pid || m.pid === finished) {
      return;
    }
    if (m.type === "splash" || m.type === "table") {
      if (!session || session.pid !== m.pid) {
        if (session && !session.done && m.type === "table" && window.rvDisplay && window.rvDisplay.active()) {
          return;	// (another server's table while ours is up: display.js stays with ours, and so do we)
        }
        begin(m.pid, m.type === "splash" ? "the window server is starting" : "a window server's table");
      }
      if (m.type === "table") {
        stage("wm");	// (the server listens: its table is there for the page)
      } else if (typeof m.stage === "string") {
        stage(m.stage);
      }
    }
  };
  const plasma = window.rvChannel("rv-plasma");
  plasma.onmessage = (event) => {
    const m = event.data;
    if (m && m.type === "config" && m.files && session) {
      configured(m);
    }
  };

  window.rvSplash = {
    state: () => ({ shown: !!el, pid: session ? session.pid : 0, stages: session ? [...session.stages] : [],
                    text: line ? line.textContent : "", theme: session && session.splash ? session.splash.name || (session.splash.none ? "None" : null) : null,
                    finished, log: log.slice() }),
    parseQml, readSplash,	// (for tests)
  };
})();
