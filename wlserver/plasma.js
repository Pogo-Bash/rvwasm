// Plasma's look for the page: the configuration files and theme files the
// page needs to draw its frames, panel and switcher the way Plasma would,
// read from the guest, and the page told when the configuration changes.
//
// The page draws everything (tools/www/plasma.js); what it cannot do is read
// the guest's files. This does it for it, read-only, and only under the
// places where Plasma keeps its look:
//
//   /usr/share/{icons,pixmaps,wallpapers,plasma,color-schemes,aurorae,fonts}
//   ~/.local/share/{icons,wallpapers,plasma,color-schemes,aurorae,fonts}
//   ~/.icons
//   /etc/xdg, ~/.config/kdedefaults, and in ~/.config the files named in CONFIGS
//
// A path is made canonical first (".." and symbolic links resolved, as the
// kernel would), and the result has to be under one of those: a link out of
// them is refused. Files are at most 32 MiB.
//
// The page asks on a BroadcastChannel of its own ("rv-plasma") and rings the
// doorbell after asking; this Worker sees the message between two turns
// (server.js yields to the browser after every turn) and answers on the same
// channel. Questions (each has an `id` the answer repeats):
//
//   { type: "config" }                      -> { type: "config", home, files: { PATH: TEXT or null } }
//   { type: "fetch", path }                 -> { type: "file", path, data (Uint8Array) } or { ..., error }
//   { type: "list", path }                  -> { type: "list", path, entries: [[NAME, "f" | "d"], ...] } or error
//   { type: "icon", name, size, themes }    -> { type: "icon", name, path, data } or { ..., error }
//
// "config" is also sent unasked, a moment after any of the files changed
// (inotify on /etc/xdg, ~/.config and ~/.config/kdedefaults: KDE's programs
// write a file next to the old one and rename it over it; System Settings
// writes several files at once, so changes are gathered for 200 ms).
//
// Also from kwinrc and breezerc: the size of the frames' title bars and
// borders (the table's title_height and border words, which this server
// places windows by and the page draws by).
(function (ws) {
  "use strict";

  const { sys1, sys3, sys4, SYS, dv, u8, path, P0, P1, S, SCRATCH, log, envp, table } = ws;
  const N_INOTIFY_INIT1 = 26, N_INOTIFY_ADD_WATCH = 27, N_FACCESSAT = 48, N_READLINKAT = 78, N_NEWFSTATAT = 79;
  const O_RDONLY = 0, O_DIRECTORY = 0o200000, O_CLOEXEC = 0o2000000, O_NONBLOCK = 0o4000;
  const AT_FDCWD = -100;
  const IN_MODIFY = 0x2, IN_CLOSE_WRITE = 0x8, IN_MOVED_TO = 0x80, IN_CREATE = 0x100, IN_DELETE = 0x200,
        IN_MOVED_FROM = 0x40, IN_IGNORED = 0x8000;
  const S_IFMT = 0o170000, S_IFREG = 0o100000, S_IFDIR = 0o040000;
  const MAX_FILE = 32 << 20;
  const EINTR = 4;

  const home = (envp.find((e) => e.startsWith("HOME=")) || "HOME=/root").slice(5).replace(/\/+$/, "") || "/root";
  const CONFIG_DIR = home + "/.config";
  // The configuration files the page follows (each in /etc/xdg, ~/.config/kdedefaults and ~/.config).
  const CONFIGS = ["kdeglobals", "kwinrc", "breezerc", "auroraerc", "plasmarc", "kcminputrc",
                   "plasma-org.kde.plasma.desktop-appletsrc", "plasmashellrc", "ksplashrc", "kxkbrc"];
  const CONFIG_DIRS = ["/etc/xdg", CONFIG_DIR + "/kdedefaults", CONFIG_DIR];
  const ROOTS = [
    ...["icons", "pixmaps", "wallpapers", "plasma", "color-schemes", "aurorae", "fonts"].map((d) => "/usr/share/" + d),
    ...["icons", "wallpapers", "plasma", "color-schemes", "aurorae", "fonts"].map((d) => home + "/.local/share/" + d),
    home + "/.icons", "/etc/xdg", CONFIG_DIR + "/kdedefaults",
  ];
  const under = (p, root) => p === root || p.startsWith(root + "/");
  // Also the wallpaper the desktop names, wherever it is (a picture in ~/Pictures: kwin.js says which, after it
  // set one, and at start): that file, or that directory if it is a wallpaper package.
  const extra = new Set();
  ws.plasmaAllow = (paths) => {
    extra.clear();
    for (const p of paths) {
      const c = canonical(p);
      if (c && c !== "/" && !c.startsWith("/proc") && !c.startsWith("/dev") && !c.startsWith("/sys")) {
        extra.add(c.replace(/\/+$/, ""));
      }
    }
  };
  const allowed = (p) => ROOTS.some((r) => under(p, r)) || CONFIGS.some((c) => p === CONFIG_DIR + "/" + c)
    || [...extra].some((e) => under(p, e));

  // ---- files

  // A path as the kernel would resolve it: absolute, no "." or "..", no symbolic links (at most 40 followed);
  // null if it cannot be resolved. (A missing last part is kept: the caller's open says what is missing.)
  const canonical = (p) => {
    if (typeof p !== "string" || p[0] !== "/" || p.length > 2048 || p.includes("\0")) {
      return null;
    }
    let todo = p.split("/").filter((x) => x && x !== ".");
    const done = [];
    let links = 0;
    while (todo.length) {
      const part = todo.shift();
      if (part === "..") {
        done.pop();
        continue;
      }
      done.push(part);
      const here = "/" + done.join("/");
      const n = sys4(N_READLINKAT, AT_FDCWD, path(here, P0), P1, 4095);
      if (n > 0) {
        if (++links > 40) {
          return null;
        }
        const target = ws.text(P1, n);
        done.pop();
        if (target[0] === "/") {
          done.length = 0;
        }
        todo = target.split("/").filter((x) => x && x !== ".").concat(todo);
      } else if (n === -2 && todo.length) {
        return null;	// (a directory on the way is missing)
      }
    }
    return "/" + done.join("/");
  };

  // [mode, size] of a path, or null.
  const stat = (p) => {
    if (sys4(N_NEWFSTATAT, AT_FDCWD, path(p, P0), S, 0) < 0) {
      return null;
    }
    const v = dv();
    return [v.getUint32(S + 16, true), Number(v.getBigInt64(S + 48, true))];
  };

  // The bytes of a file the page may read, or an error (a string).
  const fetch = (requested) => {
    const p = canonical(requested);
    if (!p || !allowed(p)) {
      return { error: "not a file the page may read" };
    }
    const st = stat(p);
    if (!st) {
      return { error: "no such file" };
    }
    if ((st[0] & S_IFMT) !== S_IFREG) {
      return { error: "not a file" };
    }
    if (st[1] > MAX_FILE) {
      return { error: "too big (" + st[1] + " bytes)" };
    }
    const fd = sys4(SYS.openat, AT_FDCWD, path(p, P0), O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
      return { error: "cannot open (error " + -fd + ")" };
    }
    const out = new Uint8Array(st[1]);
    let at = 0;
    while (at < out.length) {
      const n = sys3(SYS.read, fd, S, Math.min(SCRATCH, out.length - at));
      if (n === -EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      out.set(u8().subarray(S, S + n), at);
      at += n;
    }
    sys1(SYS.close, fd);
    return { path: p, data: at === out.length ? out : out.slice(0, at) };
  };

  // The names in a directory the page may read, with "d" for directories and "f" for the rest.
  const list = (requested, names) => {
    const p = canonical(requested);
    if (!p || !allowed(p)) {
      return { error: "not a directory the page may read" };
    }
    const fd = sys4(SYS.openat, AT_FDCWD, path(p, P0), O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0) {
      return { error: "cannot open (error " + -fd + ")" };
    }
    const entries = [];
    const decoder = new TextDecoder();
    for (;;) {
      const n = sys3(SYS.getdents64, fd, S, SCRATCH);
      if (n <= 0) {
        break;
      }
      // (A copy: stat() below uses the same scratch block, and would write over the entries still to be read.)
      const buf = u8().slice(S, S + n);
      const v = new DataView(buf.buffer);
      for (let at = 0; at + 19 < n;) {
        const reclen = v.getUint16(at + 16, true);
        if (reclen < 20) {
          break;
        }
        const type = v.getUint8(at + 18);
        let end = at + 19;
        while (end < at + reclen && buf[end]) {
          end++;
        }
        const name = decoder.decode(buf.subarray(at + 19, end));
        if (name !== "." && name !== ".." && entries.length < 4096) {
          // (DT_DIR is 4; DT_LNK 10 and DT_UNKNOWN 0 are looked at)
          let kind = type === 4 ? "d" : "f";
          if (!names && (type === 10 || type === 0)) {
            const st = stat(p + "/" + name);
            kind = st && (st[0] & S_IFMT) === S_IFDIR ? "d" : "f";
          }
          entries.push([name, kind]);
        }
        at += reclen;
      }
    }
    sys1(SYS.close, fd);
    return { path: p, entries };
  };

  const exists = (p) => sys4(N_FACCESSAT, AT_FDCWD, path(p, P0), 4, 0) === 0;

  // ---- icons, by the Icon Theme Specification (freedesktop.org, 0.13): a theme's index.theme says its
  // directories (Size, Scale, Type Fixed/Scalable/Threshold, MinSize, MaxSize, Threshold) and the themes it
  // inherits; hicolor comes last; an icon name is looked up in the directory that fits the size best.
  const ICON_BASES = [home + "/.icons", home + "/.local/share/icons", "/usr/share/icons"];
  const themes = new Map();	// name -> { bases: [dir], dirs: [{ path, size, scale, type, min, max, threshold }], inherits }
  const kconfigGroups = (text) => {
    const groups = {};
    let g = null;
    for (const raw of text.split("\n")) {
      const line = raw.trim();
      if (!line || line[0] === "#" || line[0] === ";") {
        continue;
      }
      if (line[0] === "[") {
        g = groups[line.slice(1, line.indexOf("]"))] = groups[line.slice(1, line.indexOf("]"))] || {};
        continue;
      }
      const eq = line.indexOf("=");
      if (g && eq > 0) {
        const key = line.slice(0, eq).trim();
        if (!(key in g)) {
          g[key] = line.slice(eq + 1).trim();
        }
      }
    }
    return groups;
  };
  const theme = (name) => {
    if (themes.has(name)) {
      return themes.get(name);
    }
    let t = null;
    if (/^[^/]+$/.test(name) && name !== "." && name !== "..") {
      const bases = ICON_BASES.map((b) => b + "/" + name).filter((d) => exists(d));
      const index = bases.map((b) => ws.readText(b + "/index.theme")).find((x) => x);
      if (index) {
        const g = kconfigGroups(index);
        const main = g["Icon Theme"] || {};
        const dirs = [];
        for (const d of ((main.Directories || "") + "," + (main.ScaledDirectories || "")).split(",")) {
          const e = g[d.trim()];
          if (!d.trim() || !e) {
            continue;
          }
          const size = Number(e.Size) || 0;
          dirs.push({ path: d.trim(), size, scale: Number(e.Scale) || 1, type: e.Type || "Threshold",
                      min: Number(e.MinSize) || size, max: Number(e.MaxSize) || size, threshold: Number(e.Threshold) || 2 });
        }
        t = { caches: new Map(), listed: new Map(), bases, dirs, inherits: (main.Inherits || "").split(",").map((x) => x.trim()).filter(Boolean) };
      }
    }
    themes.set(name, t);
    return t;
  };
  const sizeDistance = (d, size, scale) => {
    const s = size * scale;
    if (d.type === "Fixed") {
      return Math.abs(d.size * d.scale - s);
    }
    if (d.type === "Scalable") {
      return s < d.min * d.scale ? d.min * d.scale - s : s > d.max * d.scale ? s - d.max * d.scale : 0;
    }
    return s < (d.size - d.threshold) * d.scale ? d.min * d.scale - s
      : s > (d.size + d.threshold) * d.scale ? s - d.max * d.scale : 0;
  };
  // What files a theme directory has. A theme's icon-theme.cache (GTK's cache, which Debian's packages keep up to
  // date: gtk-update-icon-cache) says it for the whole theme in one file; without one, or with one older than
  // the theme's directory, each directory is listed when it is first needed. (Asking the kernel for every
  // candidate file instead took tens of milliseconds a question under node-machine: a theme has ~90 directories.)
  // The cache's format (gtk/gtkiconcache.c, version 1.0): big-endian; header: major, minor (u16), hash and
  // directory list offsets (u32); the directory list: a count and string offsets; the hash: a bucket count and
  // per bucket the first icon's offset; an icon: next in chain, name, image list offsets; an image list: a count
  // and per image the directory's index (u16), flags (u16: 2 .svg, 4 .png, 1 .xpm) and a data offset.
  const readCache = (base) => {
    if (!stat(base + "/icon-theme.cache")) {
      return null;
    }
    // (GTK ignores a cache older than its directory; here it is used all the same: a stale cache misses an icon
    // added since, which then comes from a theme further down. Debian's packages remake it when they change.)
    const r = fetch(base + "/icon-theme.cache");
    if (!r.data || r.data.length < 12) {
      return null;
    }
    const b = r.data, v = new DataView(b.buffer, b.byteOffset, b.length);
    const str = (at) => {
      let end = at;
      while (end < b.length && b[end]) {
        end++;
      }
      return new TextDecoder().decode(b.subarray(at, end));
    };
    try {
      if (v.getUint16(0) !== 1) {
        return null;
      }
      const hash = v.getUint32(4), dirList = v.getUint32(8);
      const dirs = [];
      for (let i = 0, n = v.getUint32(dirList); i < n; i++) {
        dirs.push(str(v.getUint32(dirList + 4 + 4 * i)));
      }
      const icons = new Map();	// name -> [[dir, flags], ...]
      let budget = 1 << 20;	// (a broken cache must not keep the server busy: a million entries at most)
      for (let i = 0, n = v.getUint32(hash); i < n && budget > 0; i++) {
        for (let at = v.getUint32(hash + 4 + 4 * i); at !== 0xffffffff && at && budget-- > 0;) {
          const name = str(v.getUint32(at + 4));
          const list = v.getUint32(at + 8);
          const images = [];
          for (let k = 0, m = v.getUint32(list); k < m; k++) {
            images.push([dirs[v.getUint16(list + 4 + 8 * k)], v.getUint16(list + 6 + 8 * k)]);
          }
          icons.set(name, images);
          at = v.getUint32(at);
        }
      }
      return icons;
    } catch (error) {
      return null;
    }
  };
  // The extension a directory of a theme has for a name, or null.
  const has = (t, base, dir, name) => {
    let c = t.caches.get(base);
    if (c === undefined) {
      c = readCache(base);
      t.caches.set(base, c);
    }
    if (c) {
      const images = c.get(name);
      const hit = images && images.find((x) => x[0] === dir);
      return hit ? (hit[1] & 2 ? ".svg" : hit[1] & 4 ? ".png" : null) : null;
    }
    const key = base + "/" + dir;
    let files = t.listed.get(key);
    if (!files) {
      const r = list(key, true);
      files = new Set(r.entries ? r.entries.map((e) => e[0]) : []);
      t.listed.set(key, files);
    }
    for (const ext of [".svg", ".svgz", ".png"]) {
      if (files.has(name + ext)) {
        return ext;
      }
    }
    return null;
  };
  const lookupIn = (t, name, size, scale) => {
    if (!t.order) {
      t.order = new Map();
    }
    const key = size + "x" + scale;
    let order = t.order.get(key);
    if (!order) {
      // (the spec: directories of the scale asked for first, then by how far their size is)
      order = t.dirs.map((d) => [d.scale === scale ? 0 : 1, sizeDistance(d, size, scale), d.type === "Scalable" ? 0 : 1, d])
        .sort((a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2]).map((x) => x[3]);
      t.order.set(key, order);
    }
    for (const d of order) {
      for (const base of t.bases) {
        const ext = has(t, base, d.path, name);
        if (ext) {
          return base + "/" + d.path + "/" + name + ext;
        }
      }
    }
    return null;
  };
  const iconCache = new Map();
  const findIcon = (name, size, scale, chain) => {
    if (typeof name !== "string" || !name || name.length > 255) {
      return null;
    }
    if (name[0] === "/") {
      return name;	// (an Icon= with a path: fetch() decides whether it may be read)
    }
    name = name.replace(/\.(png|svg|svgz|xpm)$/, "");
    if (name.includes("/")) {
      return null;
    }
    const key = name + "@" + size + "x" + scale + ":" + chain.join(",");
    if (iconCache.has(key)) {
      return iconCache.get(key);
    }
    // The themes in order: the ones asked for, each followed by what it inherits (depth first), hicolor last.
    const seen = new Set(), order = [];
    const visit = (n, depth) => {
      if (seen.has(n) || depth > 10) {
        return;
      }
      seen.add(n);
      const t = theme(n);
      if (t) {
        order.push(t);
        for (const i of t.inherits) {
          visit(i, depth + 1);
        }
      }
    };
    for (const n of chain) {
      visit(n, 0);
    }
    visit("hicolor", 0);
    let found = null;
    // A name with dashes falls back to its shorter forms (the spec's "generic" fallback: a-b-c, a-b, a).
    for (let n = name; n && !found; n = n.includes("-") ? n.slice(0, n.lastIndexOf("-")) : "") {
      for (const t of order) {
        found = lookupIn(t, n, size, scale);
        if (found) {
          break;
        }
      }
    }
    if (!found) {
      for (const base of ["/usr/share/pixmaps"]) {
        for (const ext of [".svg", ".png"]) {
          if (!found && exists(base + "/" + name + ext)) {
            found = base + "/" + name + ext;
          }
        }
      }
    }
    if (iconCache.size > 4000) {
      iconCache.clear();
    }
    iconCache.set(key, found);
    return found;
  };

  // ---- the configuration

  const configFiles = () => {
    const files = {};
    for (const dir of CONFIG_DIRS) {
      for (const name of CONFIGS) {
        files[dir + "/" + name] = ws.readText(dir + "/" + name);
      }
    }
    return files;
  };

  // The title bar's height and the border, as KWin's decorations have them. The page draws by these two
  // words of the table and the server places windows by them, so they are worked out here, once, from
  // the same files the page reads. The numbers, from plasma/breeze kdecoration/ (breezedecoration.cpp,
  // breeze.h) and plasma/kdecoration (decorationsettings.cpp), design values only:
  //   gridUnit     = the height of an "M" in the title font, rounded to an even number
  //   smallSpacing = max(2, gridUnit / 4)
  //   button       = gridUnit x 1, 1.5, 2 (the default), 2.5 or 3.5 (breezerc [Windeco] ButtonSize)
  //   title bar    = max(the font's height, button) + 2 x smallSpacing above and below
  //   border       = smallSpacing x 0, 1, 2, 3, 4, 5, 6 or 10 for None ... Oversized; Breeze recommends None,
  //                  which KWin takes while kwinrc says nothing else (BorderSizeAuto)
  // Fonts cannot be measured here: an "M" is 0.714 of the size in pixels and a line 1.362 of it (Noto Sans,
  // Plasma's font; other fonts come out near enough). An Aurorae theme says its sizes in its NAMErc.
  // The border word is at least 4: what the page draws is the configured border, the rest of the 4 pixels
  // is a band to resize by that shows what is under it (KWin's resize area outside a borderless window).
  const pickIn = (files, file, group, key) => {
    let value = null;
    for (const dir of CONFIG_DIRS) {
      const text = files[dir + "/" + file];
      if (text) {
        const g = kconfigGroups(text)[group];
        if (g && g[key] !== undefined) {
          value = g[key];
        }
      }
    }
    return value;
  };
  const BORDERS = { None: 0, NoSides: 0, Tiny: 1, Normal: 2, Large: 3, VeryLarge: 4, Huge: 5, VeryHuge: 6, Oversized: 10 };
  const auroraeTheme = (name) => {
    for (const base of [home + "/.local/share/aurorae/themes/", "/usr/share/aurorae/themes/"]) {
      const text = /^[^/]+$/.test(name) ? ws.readText(base + name + "/" + name + "rc") : null;
      if (text) {
        return kconfigGroups(text);
      }
    }
    return null;
  };
  const metrics = (files) => {
    const deco = (key) => pickIn(files, "kwinrc", "org.kde.kdecoration2", key);
    const library = deco("library") || "org.kde.breeze";
    const themeName = deco("theme") || "";
    const auto = deco("BorderSizeAuto") !== "false";
    const aurorae = library === "org.kde.kwin.aurorae" && themeName.startsWith("__aurorae__svg__")
      ? auroraeTheme(themeName.slice("__aurorae__svg__".length)) : null;
    const size = auto ? (aurorae ? "Normal" : "None") : deco("BorderSize") || "Normal";
    if (aurorae) {
      // Aurorae's sizes (kwin's aurorae/src/lib/auroraetheme.cpp, borders()): the title bar is the edges and the
      // taller of TitleHeight and the buttons (ButtonHeight and ButtonMarginTop); a border is the theme's, held
      // between a least and a most for the border size kwinrc asks for, none for None (and no sides for NoSides).
      const l = aurorae.Layout || {};
      const n = (k, d) => (l[k] === undefined || isNaN(Number(l[k])) ? d : Number(l[k]));
      const title = n("TitleEdgeTop", 5) + Math.max(n("TitleHeight", 20), n("ButtonHeight", 20) + n("ButtonMarginTop", 0)) + n("TitleEdgeBottom", 5);
      const clamp = { Tiny: [1, 4], NoSides: [1, 4], Normal: [4, 6], Large: [6, 8], VeryLarge: [8, 12], Huge: [12, 20],
                      VeryHuge: [23, 30], Oversized: [36, 48] }[size] || [0, 0];
      const side = (k) => Math.max(clamp[0], Math.min(clamp[1], n(k, 5)));
      const border = size === "None" ? 0 : Math.max(size === "NoSides" ? 0 : Math.max(side("BorderLeft"), side("BorderRight")), side("BorderBottom"));
      return { title: Math.max(10, Math.min(120, title)), border: Math.max(4, Math.min(48, border)) };
    }
    const fontText = pickIn(files, "kdeglobals", "WM", "activeFont") || pickIn(files, "kdeglobals", "General", "font")
      || "Noto Sans,10,-1,5,400,0,0,0,0,0";
    const parts = fontText.split(",");
    const px = Number(parts[2]) > 0 ? Number(parts[2]) : (Number(parts[1]) || 10) * 96 / 72;
    const gridUnit = Math.max(2, 2 * Math.round(0.714 * px / 2));
    const small = Math.max(2, Math.floor(gridUnit / 4));
    const factor = { ButtonTiny: 1, ButtonSmall: 1.5, ButtonDefault: 2, ButtonLarge: 2.5, ButtonVeryLarge: 3.5 }[
      pickIn(files, "breezerc", "Windeco", "ButtonSize") || "ButtonDefault"] || 2;
    const title = Math.max(Math.ceil(1.362 * px), Math.round(gridUnit * factor)) + 4 * small;
    const step = BORDERS[size] === undefined ? 2 : BORDERS[size];
    const border = step * small;
    return { title: Math.max(10, Math.min(80, title)), border: Math.max(4, Math.min(40, border)) };
  };

  const channel = ws.channel("rv-plasma");
  let lastConfig = "";
  const sendConfig = (force, id) => {
    const files = configFiles();
    const json = JSON.stringify(files);
    if (!force && json === lastConfig) {
      return;
    }
    lastConfig = json;
    const m = metrics(files);
    if (m.title !== ws.chrome.title || m.border !== ws.chrome.border) {
      ws.chrome.title = m.title;
      ws.chrome.border = m.border;
      dv().setUint32(table.address + 136, m.title, true);	// (table.js: H_TITLE_HEIGHT, H_BORDER)
      dv().setUint32(table.address + 140, m.border, true);
      log("plasma: frames now have a title bar of " + m.title + " and a border of " + m.border + " pixels");
      if (ws.outputChanged) {
        ws.outputChanged();	// (the windows fitted to the screen again, with frames of the new size: shell.js)
      }
    }
    channel.postMessage({ type: "config", id, home, files });
  };

  // The page's questions.
  channel.onmessage = (event) => {
    const q = event.data;
    if (!q || typeof q !== "object") {
      return;
    }
    try {
      if (q.type === "config") {
        sendConfig(true, q.id);
      } else if (q.type === "fetch") {
        channel.postMessage({ type: "file", id: q.id, path: q.path, ...fetch(q.path) });
      } else if (q.type === "list") {
        channel.postMessage({ type: "list", id: q.id, ...list(q.path) });
      } else if (q.type === "icon") {
        const t0 = performance.now();
        const chain = Array.isArray(q.themes) ? q.themes.filter((x) => typeof x === "string").slice(0, 8) : ["breeze"];
        const p = findIcon(q.name, Math.max(8, Math.min(512, q.size | 0 || 32)), Math.max(1, Math.min(3, q.scale | 0 || 1)), chain);
        const r = p ? fetch(p) : { error: "no such icon" };
        channel.postMessage({ type: "icon", id: q.id, name: q.name, ...r });
        if (ws.debug || performance.now() - t0 > 500) {
          log("plasma: icon " + q.name + " -> " + (p || "none") + " in " + Math.round(performance.now() - t0) + " ms");
        }
      }
    } catch (error) {
      channel.postMessage({ type: "error", id: q.id, error: String(error && error.message || error) });
    }
  };

  // ---- watching: inotify on the configuration directories (and on the home directory and ~/.config, for the
  // directories that are not there yet).
  const ino = sys1(N_INOTIFY_INIT1, O_NONBLOCK | O_CLOEXEC);
  const watched = new Map();	// wd -> directory
  const watchDir = (dir) => {
    if ([...watched.values()].includes(dir)) {
      return;
    }
    const wd = sys3(N_INOTIFY_ADD_WATCH, ino, path(dir, P0),
                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_MODIFY);
    if (wd >= 0) {
      watched.set(wd, dir);
    }
  };
  let soon = 0;
  const changed = () => {
    clearTimeout(soon);
    soon = setTimeout(() => sendConfig(false), 200);
  };
  if (ino >= 0) {
    for (const dir of [home, ...CONFIG_DIRS]) {
      watchDir(dir);
    }
    ws.watchFd(ino, () => {
      for (;;) {
        const n = sys3(SYS.read, ino, S, SCRATCH);
        if (n <= 0) {
          return;
        }
        const v = dv();
        for (let at = S; at < S + n;) {
          const wd = v.getInt32(at, true), mask = v.getUint32(at + 4, true), len = v.getUint32(at + 12, true);
          const name = len ? ws.cstring(at + 16) : "";
          const dir = watched.get(wd);
          if (mask & IN_IGNORED) {
            watched.delete(wd);
          } else if (dir) {
            const full = dir + "/" + name;
            if (CONFIG_DIRS.includes(full)) {
              watchDir(full);	// (~/.config or kdedefaults made just now)
              changed();
            } else if (CONFIG_DIRS.includes(dir) && CONFIGS.includes(name)) {
              changed();
            }
          }
          at += 16 + len;
        }
      }
    });
  } else {
    log("plasma: no inotify (error " + -ino + "): the configuration is read when the page asks");
  }

  sendConfig(true);
  log("plasma: files for the page under " + ROOTS.length + " places, configuration in " + CONFIG_DIRS.join(", "));
  ws.plasma = { canonical, allowed, fetch, list, findIcon, configFiles };	// (for tests)
})
