// Window rules: ~/.config/kwinrulesrc, as KWin keeps it (System Settings'
// Window Rules page writes it, then sends the reloadConfig signal, which
// kwin.js hears; KWin 6.3, src/rules.cpp, src/rulesettings.kcfg).
//
// The file: [General] count and rules (the rules' group names, in order),
// then a group per rule. A rule matches a window by its window class
// (wmclass, with wmclassmatch 0 unimportant, 1 exact, 2 substring, 3 regular
// expression; wmclasscomplete: match "name class" rather than the class),
// its title (title, titlematch), its role (windowrole, windowrolematch: X
// windows' WM_WINDOW_ROLE) and its type (types, NET's masks: 1 normal, 32
// dialog, 256 utility, ...; all when it is not there). A disabled rule
// (Enabled=false) matches nothing. The window class of a Wayland window is
// its app id (org.kde.konsole), its name the last part of that (konsole);
// an X window's are WM_CLASS's class and instance, in lower case, as KWin
// has them.
//
// What a matching rule sets, each with its NAMErule word (1 do not affect,
// 2 force, 3 apply initially, 4 remember, 5 apply now, 6 force temporarily;
// 0 or missing: not used): position (x,y of the frame's corner), size (w,h
// of the frame), desktops (desktop ids; none: all desktops), maximizehoriz
// and maximizevert (both: maximized), minimize, shade, fullscreen, above,
// below, noborder, skiptaskbar, skippager, skipswitcher. The first rule that
// sets a property wins (KWin goes through the rules in order the same way).
// All of them are carried out when a window first maps; "force" is not held
// against later changes (a forced position can be dragged away), which is
// where this differs from KWin. Rules the page cannot show (opacity, the
// decoration's colour, shortcuts, focus stealing, layers) are left alone.
(function (ws) {
  "use strict";

  const { log, chrome, F } = ws;
  const K = ws.kconfig;
  const PROPS = ["position", "size", "desktops", "maximizehoriz", "maximizevert", "minimize", "shade", "fullscreen",
                 "above", "below", "noborder", "skiptaskbar", "skippager", "skipswitcher"];
  const MASK = { normal: 1, desktop: 2, dock: 4, toolbar: 8, menu: 16, dialog: 32, override: 64, topmenu: 128,
                 utility: 256, splash: 512 };

  let rules = [];

  const load = () => {
    const cfg = K.readFile("kwinrulesrc");
    let names = (K.get(cfg, "General", "rules", "") || "").split(",").map((x) => x.trim()).filter(Boolean);
    if (!names.length) {
      // (older files: groups 1..count)
      const count = K.int(cfg, "General", "count", 0);
      for (let i = 1; i <= count; i++) {
        names.push(String(i));
      }
    }
    rules = [];
    for (const name of names) {
      const g = cfg.groups.get(name);
      if (!g) {
        continue;
      }
      const r = { name, description: g.get("Description") || g.get("description") || name, keys: g };
      if (/^false$/i.test(g.get("Enabled") || "true")) {
        continue;
      }
      rules.push(r);
    }
    log("rules: " + rules.length + " window rules" + (rules.length ? " (" + rules.map((r) => r.description).join(", ") + ")" : ""));
  };

  const matches = (kind, pattern, text) => {
    switch (kind) {
    case 1:
      return text === pattern;
    case 2:
      return text.includes(pattern);
    case 3:
      try {
        return new RegExp(pattern).test(text);
      } catch (error) {
        return false;
      }
    default:
      return true;
    }
  };

  // What KWin knows a window by: { cls, name, title, role, type }.
  const describe = (surface) => {
    if (surface.xwin && ws.xwmDescribe) {
      return ws.xwmDescribe(surface);
    }
    const appId = surface.appId || "";
    return { cls: appId, name: appId.split(".").pop(), title: surface.title || "", role: "",
             type: surface.top && surface.top.parent ? "dialog" : "normal" };
  };

  const ruleMatches = (r, d) => {
    const k = r.keys;
    const int = (key) => parseInt(k.get(key) || "0", 10) || 0;
    const types = k.has("types") ? Number(k.get("types")) >>> 0 : 0xffffffff;
    if (types !== 0xffffffff && !(types & (MASK[d.type] || 1))) {
      return false;
    }
    const complete = /^true$/i.test(k.get("wmclasscomplete") || "false");
    if (!matches(int("wmclassmatch"), k.get("wmclass") || "", complete ? d.name + " " + d.cls : d.cls)) {
      return false;
    }
    if (!matches(int("windowrolematch"), k.get("windowrole") || "", d.role)) {
      return false;
    }
    return matches(int("titlematch"), k.get("title") || "", d.title);
  };

  // The properties the matching rules set: { name: text }, the first rule's for each.
  const wanted = (surface) => {
    const d = describe(surface);
    const out = {};
    const from = {};
    for (const r of rules) {
      if (!ruleMatches(r, d)) {
        continue;
      }
      for (const p of PROPS) {
        const how = parseInt(r.keys.get(p + "rule") || "0", 10) || 0;
        if (how >= 2 && !(p in out)) {
          out[p] = r.keys.get(p) || "";
          from[p] = r.description;
        }
      }
    }
    return { set: out, from, d };
  };

  const pair = (text) => {
    const m = /^\s*(-?\d+)\s*,\s*(-?\d+)\s*$/.exec(text || "");
    return m ? [Number(m[1]), Number(m[2])] : null;
  };
  const yes = (text) => /^true$/i.test(text || "");

  // Once per window, when it is about to be shown first (shell.js: before its first configure, and when it maps).
  ws.applyRules = (surface) => {
    const t = surface.top;
    if (!t || t.rulesDone || !rules.length) {
      return;
    }
    t.rulesDone = true;
    const { set, from, d } = wanted(surface);
    const names = Object.keys(set);
    if (!names.length) {
      return;
    }
    log("rules: " + (d.cls || d.title) + ": " + names.map((n) => n + "=" + set[n] + " (" + from[n] + ")").join(", "));
    if ("noborder" in set) {
      t.noBorder = yes(set.noborder);
    }
    const decorated = () => ws.decorated(surface);
    if ("size" in set) {
      const s = pair(set.size);
      if (s && s[0] > 0 && s[1] > 0) {
        // (the frame's size: what is inside our frame is the window's geometry)
        t.size = decorated() ? [Math.max(1, s[0] - 2 * chrome.border), Math.max(1, s[1] - chrome.title - 2 * chrome.border)] : s;
        ws.reconfigure(surface);
      }
    }
    if ("position" in set) {
      const p = pair(set.position);
      if (p) {
        t.wx = p[0] + (decorated() ? chrome.border : 0);
        t.wy = p[1] + (decorated() ? chrome.title + chrome.border : 0);
        t.placed = true;
      }
    }
    if ("desktops" in set) {
      const ids = set.desktops.split(",").map((x) => x.trim()).filter(Boolean);
      if (!ids.length) {
        t.vd = null;	// (all desktops)
      } else {
        const known = ws.desktops.list.find((dd) => ids.includes(dd.id));
        if (known) {
          t.vd = known.id;
        }
      }
    }
    for (const [p, key] of [["above", "above"], ["below", "below"], ["shade", "shaded"], ["skiptaskbar", "skipTaskbar"],
                            ["skippager", "skipPager"], ["skipswitcher", "skipSwitcher"]]) {
      if (p in set) {
        t[key] = yes(set[p]);
      }
    }
    if (t.above && t.below) {
      t.below = false;
    }
    if (yes(set.maximizehoriz) && yes(set.maximizevert)) {
      ws.setMaximized(surface, true);
    }
    if (yes(set.fullscreen)) {
      ws.setFullscreen(surface, true);
    }
    if (yes(set.minimize)) {
      surface.flags |= F.MINIMIZED;
    }
    surface.dirty = true;
    if (ws.statesChanged) {
      ws.statesChanged(surface);
    }
  };

  ws.rulesReload = load;
  ws.rulesFor = (surface) => wanted(surface);	// (for tests)
  load();
})
