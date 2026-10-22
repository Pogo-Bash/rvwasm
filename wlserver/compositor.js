// What the messages mean: the globals, the objects a client makes, surfaces
// and their buffers, and the surface table's side of it all. (Windows, the
// shell's part, are in shell.js; input in seat.js; the clipboard in data.js.)
//
// A surface's pixels are a wl_buffer: a piece of a wl_shm_pool, which is a
// file the client sent a descriptor for. The pool is mapped here, shared,
// and on this kernel (no MMU) the address the mapping gets is where the
// file's pages are in the one memory of the machine: the page can draw from
// that address directly. So a commit costs a few words in the table and no
// copy of pixels, and the page uploads only the rectangles the client said
// it changed.
//
// What the page has uploaded it says in the table; on every turn the server
// looks, fires the frame callbacks of the presents the page has shown, and
// releases the buffers it has finished reading (wl_buffer.release), which is
// what lets a double-buffered client draw its next frame.
(function (ws) {
  "use strict";

  const { send, log, sys1, sys2, sys3, sys4, sys6, SYS, table, screen } = ws;

  const PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 1;
  const KIND_TOPLEVEL = 1, KIND_POPUP = 2, KIND_SUBSURFACE = 3, KIND_DRAG_ICON = 5, KIND_LAYER = 6;
  const F_MAPPED = 1, F_ACTIVATED = 2, F_MAXIMIZED = 4, F_FULLSCREEN = 8, F_RESIZING = 16, F_DECORATED = 32,
        F_MINIMIZED = 64;
  // The slot's shell_flags (PLASMA-PROTOCOL.md)
  const SH_ABOVE = 1, SH_BELOW = 2, SH_SHADED = 4, SH_SKIP_TASKBAR = 8, SH_SKIP_SWITCHER = 16, SH_ATTENTION = 32,
        SH_TILED = 64, SH_NO_BORDER = 128;
  const FORMAT_ARGB8888 = 0, FORMAT_XRGB8888 = 1;
  const SERVER_IDS = 0xff000000;

  let serial = 0;
  const nextSerial = () => {
    serial = (serial + 1) >>> 0;
    return serial || ++serial;
  };
  ws.nextSerial = nextSerial;

  const now = () => (performance.now() | 0) >>> 0;

  // ---- objects

  ws.create = (client, id, iface, version) => {
    const object = { id, interface: iface, version, client, onDestroy: null };
    client.objects.set(id, object);
    return object;
  };

  ws.destroy = (object) => {
    const client = object.client;
    if (!client || client.objects.get(object.id) !== object) {
      return;
    }
    client.objects.delete(object.id);
    if (object.onDestroy) {
      const f = object.onDestroy;
      object.onDestroy = null;
      f();
    }
    if (object.id < SERVER_IDS && !client.gone) {
      send(client.display, "delete_id", object.id);
    }
  };

  // ---- globals

  const globals = [];
  // (only: a function of the client, for a global that only some clients are told of: xwayland_shell_v1 is
  // Xwayland's alone.)
  const global = (iface, version, bind, only = null) => {
    globals.push({ name: globals.length + 1, interface: iface, version, bind, only });
  };

  // ---- surfaces, in the order the page draws them (the bottom first)

  const windows = [];		// surfaces with a slot
  let surfaceIds = 0;

  const restack = () => {
    windows.forEach((s, i) => {
      if (s.z !== i) {
        s.z = i;
        s.dirty = true;
      }
    });
  };

  const newSurface = (object) => {
    const surface = {
      object, id: ++surfaceIds, role: null, xdg: null, sub: null,
      pending: freshState(), current: { buffer: null, scale: 1 },
      presentSeq: 0, presents: [], busy: new Set(), waiting: [],	// (waiting: frame callbacks of a surface the page does not show)
      slot: -1, z: 0, x: 0, y: 0, geometry: [0, 0, 0, 0], flags: 0, title: "", appId: "", dirty: false,
      children: [], input: null, viewport: null, top: null, kdeMode: 0,
    };
    object.surface = surface;
    object.onDestroy = () => {
      unmap(surface);
      surface.destroyed = true;
      if (ws.xwmSurfaceGone && surface.role === "xwayland") {
        ws.xwmSurfaceGone(surface);
      }
      if (ws.surfaceGone) {
        ws.surfaceGone(surface);
      }
      for (const cb of surface.pending.frames) {
        ws.destroy(cb);
      }
      if (surface.sub && surface.sub.parent) {
        const siblings = surface.sub.parent.children;
        const at = siblings.indexOf(surface);
        if (at >= 0) {
          siblings.splice(at, 1);
        }
      }
      for (const child of surface.children.slice()) {
        child.sub.parent = null;
        unmap(child);
      }
    };
    return surface;
  };

  const freshState = () => ({ buffer: undefined, damage: [], bufferDamage: [], frames: [] });

  // A surface's size on the screen: its buffer's over its scale, or what its viewport says.
  const size = (surface) => {
    const vp = surface.viewport;
    if (vp && vp.dst) {
      return vp.dst;
    }
    if (vp && vp.src) {
      return [Math.round(vp.src[2]), Math.round(vp.src[3])];
    }
    const b = surface.current.buffer;
    if (!b) {
      return [0, 0];
    }
    return [Math.floor(b.width / surface.current.scale), Math.floor(b.height / surface.current.scale)];
  };
  ws.size = size;

  // The window geometry (xdg_surface.set_window_geometry), or all of the surface when none was set.
  ws.geometry = (surface) => {
    if (surface.xwin && ws.xwmGeometry) {
      const xg = ws.xwmGeometry(surface);	// (an X window that draws its own shadows says where they are)
      if (xg) {
        return xg;
      }
    }
    const g = surface.geometry;
    if (g[2] > 0 && g[3] > 0) {
      return g;
    }
    const [w, h] = size(surface);
    return [0, 0, w, h];
  };

  // Where a surface's top left corner is on the screen (shell.js knows where windows and popups are).
  ws.screenPos = (surface) => {
    if (surface.sub) {
      const p = surface.sub.parent ? ws.screenPos(surface.sub.parent) : [0, 0];
      return [p[0] + surface.sub.x, p[1] + surface.sub.y];
    }
    if (surface.xdg && ws.shellPos) {
      return ws.shellPos(surface);
    }
    if (surface.xwin && ws.xwmPos) {
      return ws.xwmPos(surface);	// (an X window's: xwm.js)
    }
    if (surface.role === "dnd_icon" && ws.dragIconPos) {
      return ws.dragIconPos(surface);
    }
    return [surface.x, surface.y];
  };

  // The window a surface belongs to: a subsurface's parent's, a popup's parent's, the surface itself.
  ws.root = (surface) => {
    for (let s = surface, n = 0; s && n < 64; n++) {
      if (s.sub && s.sub.parent) {
        s = s.sub.parent;
      } else if (s.xdg && s.xdg.popup && s.xdg.popup.parent) {
        s = s.xdg.popup.parent;
      } else {
        return s;
      }
    }
    return surface;
  };

  // Is the point (x, y), in the surface's own coordinates, in its input region?
  ws.accepts = (surface, x, y) => {
    const [w, h] = size(surface);
    if (x < 0 || y < 0 || x >= w || y >= h) {
      return false;
    }
    if (!surface.input) {
      return true;
    }
    let inside = false;
    for (const r of surface.input) {
      if (x >= r[1] && y >= r[2] && x < r[1] + r[3] && y < r[2] + r[4]) {
        inside = r[0] === 1;
      }
    }
    return inside;
  };

  // ---- shm

  const mapPool = (pool) => {
    const r = sys6(SYS.mmap, 0, pool.size, PROT_READ | PROT_WRITE, MAP_SHARED, pool.fd, 0);
    if (r < 0 && r > -4096) {
      log("shm: cannot map pool of " + pool.size + " bytes (error " + -r + "): its buffers will be copied");
      pool.address = 0;
      return;
    }
    pool.address = r;
  };

  const unmapPool = (pool) => {
    if (pool.address) {
      sys2(SYS.munmap, pool.address, pool.size);
      pool.address = 0;
    }
  };

  const poolDone = (pool) => {
    if (pool.destroyed && pool.buffers === 0) {
      unmapPool(pool);
      sys1(SYS.close, pool.fd);
      pool.fd = -1;
    }
  };

  // Where a buffer's pixels are: in the pool's mapping, or (a pool that could not be mapped) in a copy of
  // this process's own, read from the file now.
  const bufferAddress = (buffer) => {
    if (buffer.gpu) {
      return buffer.gpu.res + buffer.gpu.pid * 0x100000000;	// (a GPU resource: dmabuf.js; the slot says format 2)
    }
    const pool = buffer.pool;
    if (pool.address) {
      return pool.address + buffer.offset;
    }
    const size = buffer.stride * buffer.height;
    if (!buffer.copy) {
      buffer.copy = ws.alloc(size);
    }
    let got = 0;
    while (got < size) {
      const n = sys4(SYS.pread64, pool.fd, buffer.copy + got, size - got, buffer.offset + got);
      if (n <= 0) {
        break;
      }
      got += n;
    }
    return buffer.copy;
  };

  // ---- the table's side

  const map = (surface) => {
    if (surface.slot >= 0) {
      return true;
    }
    const slot = table.take();
    if (slot < 0) {
      log("compositor: no slot left for a window (" + table.SLOTS + " in use)");
      return false;
    }
    surface.slot = slot;
    surface.fresh = true;
    surface.flags |= F_MAPPED;
    if (surface.sub && surface.sub.parent && surface.sub.parent.slot >= 0) {
      // Right above its parent (and its parent's earlier children).
      let at = windows.indexOf(surface.sub.parent) + 1;
      while (at < windows.length && isDescendant(windows[at], surface.sub.parent)) {
        at++;
      }
      windows.splice(at, 0, surface);
    } else {
      windows.push(surface);
    }
    restack();
    surface.dirty = true;
    return true;
  };

  const isDescendant = (surface, ancestor) => {
    for (let s = surface; s && s.sub; s = s.sub.parent) {
      if (s.sub.parent === ancestor) {
        return true;
      }
    }
    return false;
  };

  const unmap = (surface) => {
    if (surface.slot < 0) {
      return;
    }
    table.release(surface.slot);
    surface.slot = -1;
    surface.flags &= ~F_MAPPED;
    const at = windows.indexOf(surface);
    if (at >= 0) {
      windows.splice(at, 1);
    }
    restack();
    // What the page will never show: let the client go on anyway.
    for (const p of surface.presents) {
      surface.waiting.push(...p.frames);
    }
    surface.presents = [];
    for (const b of surface.busy) {
      send(b, "release");
    }
    surface.busy.clear();
    for (const child of surface.children) {
      unmap(child);
    }
    if (ws.unmapped) {
      ws.unmapped(surface);
    }
  };

  // Write a surface's slot: its place, its current present, and the damage the page has not taken yet.
  const writeSlot = (surface) => {
    const buffer = surface.current.buffer;
    const latest = surface.presents[surface.presents.length - 1];
    const acked = table.read(surface.slot);
    let damage = [];
    for (const p of surface.presents) {
      if (p.id > acked.uploaded) {
        if (p.damage === null) {
          damage = null;
          break;
        }
        damage.push(...p.damage);
      }
    }
    let x, y;
    if (surface.sub) {
      x = surface.sub.x;
      y = surface.sub.y;
    } else {
      [x, y] = ws.screenPos(surface);
    }
    const [w, h] = size(surface);
    let flags = surface.flags;
    if (surface.top && ws.decorated(surface)) {
      flags |= F_DECORATED;
    }
    if (surface.layer && surface.layer.hidden) {
      flags |= F_MINIMIZED;	// (a panel hidden at its screen edge, layershell.js: not drawn, no input)
    }
    // KWin's states of the window this surface is of (PLASMA-PROTOCOL.md): its desktop (1-based, 0 on all; a
    // window's subsurfaces and popups carry their window's, so that the page hides them with it), shell_flags
    // and its quick tile.
    const r = ws.root(surface);
    const rt = r.top;
    let desktop = r.layer ? 0 : 1, shellFlags = 0, tile = 0;	// (a layer surface is on every desktop)
    if (rt) {
      const d = ws.windowDesktop ? ws.windowDesktop(r) : 0;	// (0-based, -1 on all desktops: desktops.js)
      desktop = d < 0 ? 0 : d + 1;
      shellFlags = (rt.above ? SH_ABOVE : 0) | (rt.below ? SH_BELOW : 0) | (rt.shaded ? SH_SHADED : 0)
        | (rt.skipTaskbar ? SH_SKIP_TASKBAR : 0) | (rt.skipSwitcher ? SH_SKIP_SWITCHER : 0) | (rt.attention ? SH_ATTENTION : 0)
        | (rt.tile && rt.tile !== 15 ? SH_TILED : 0) | (rt.noBorder ? SH_NO_BORDER : 0);
      tile = rt.tile || 0;
    }
    if (surface.xwin && ws.xwmSync) {
      ws.xwmSync(surface);	// (the X window is moved to where it is drawn: xwm.js)
    }
    const xpopup = surface.xwin && !surface.top;	// (an X menu, tooltip, ...: no frame, above the windows)
    table.write(surface.slot, {
      id: surface.id,
      parent: surface.sub && surface.sub.parent ? surface.sub.parent.id :
        surface.xdg && surface.xdg.popup && surface.xdg.popup.parent ? surface.xdg.popup.parent.id :
        xpopup && ws.xwmParent ? ws.xwmParent(surface) : 0,
      kind: surface.sub ? KIND_SUBSURFACE : (surface.xdg && surface.xdg.popup) || xpopup ? KIND_POPUP
        : surface.role === "dnd_icon" ? KIND_DRAG_ICON : surface.layer ? KIND_LAYER : KIND_TOPLEVEL,
      x, y, w, h,
      z: surface.z, flags, geometry: (surface.layer && ws.layerVisible && ws.layerVisible(surface)) || ws.geometry(surface),
      present: latest ? latest.id : surface.presentSeq,
      buffer: buffer ? bufferAddress(buffer) : 0,
      stride: buffer ? buffer.stride : 0,
      format: buffer ? buffer.format : 0,
      bufferWidth: buffer ? buffer.width : 0,
      bufferHeight: buffer ? buffer.height : 0,
      scale: surface.current.scale,
      gpuSerial: latest && latest.gpuSerial ? latest.gpuSerial : 0,
      damage,
      title: surface.title, appId: surface.appId, desktop, shellFlags, tile,
      // (a layer surface's layer, 0 background .. 3 overlay; a window's role in Plasma's shell, plasmashell.js)
      layer: surface.layer ? surface.layer.state.layer : 0,
      role: rt && rt.plasmaRole ? rt.plasmaRole : surface.layer ? ws.layerRole(surface) : 0,
    });
    surface.dirty = false;
  };

  // ---- commit

  const commit = (surface) => {
    const st = surface.pending;
    surface.pending = freshState();
    const xdg = surface.xdg;
    let changed = false;
    if (st.buffer !== undefined) {
      if (st.buffer !== surface.current.buffer) {
        changed = true;
      }
      surface.current.buffer = st.buffer;
      if (st.buffer) {
        surface.busy.add(st.buffer);
      }
    }
    if (st.scale !== undefined) {
      surface.current.scale = st.scale;
    }
    if (st.input !== undefined) {
      surface.input = st.input;
    }
    if (st.viewport !== undefined) {
      surface.viewport = st.viewport;
    }
    const buffer = surface.current.buffer;

    if (surface.role === "cursor") {
      ws.cursorCommit(surface, st);
      return;
    }
    if (surface.role === "dnd_icon") {
      ws.dragIconCommit(surface, st);
    }
    // The shell's first word: a surface with a role but no buffer yet asks to be configured.
    if (surface.layer && ws.layerCommit(surface, buffer) === false) {
      surface.waiting.push(...st.frames);
      return;
    }
    if (xdg && ws.shellCommit(surface, buffer) === false) {
      surface.waiting.push(...st.frames);
      return;
    }
    if (!buffer) {
      unmap(surface);
      surface.waiting.push(...st.frames);
      return;
    }
    if (!placeable(surface)) {
      surface.waiting.push(...st.frames);
      // Nobody will read the buffers it had before this one (an X window's surface draws before the window
      // manager has heard of it): they go back to the client.
      for (const b of surface.busy) {
        if (b !== buffer) {
          surface.busy.delete(b);
          if (!b.destroyed) {
            send(b, "release");
          }
        }
      }
      return;
    }
    const fresh = surface.slot < 0;
    if (fresh && !map(surface)) {
      surface.waiting.push(...st.frames);
      return;
    }
    if (fresh) {
      // On the one output there is (Qt and GTK pick their scale by the outputs a surface is on).
      for (const output of outputsOf(surface.object.client)) {
        send(surface.object, "enter", output);
      }
      if (ws.mapped) {
        ws.mapped(surface);
      }
    }
    // Damage, in buffer pixels, within the buffer.
    let damage = [];
    for (const r of st.damage) {
      damage.push(surfaceToBuffer(surface, buffer, r));
    }
    damage.push(...st.bufferDamage);
    damage = damage.map((r) => clip(r, buffer.width, buffer.height)).filter((r) => r[2] > 0 && r[3] > 0);
    if (changed && damage.length === 0) {
      damage = null;	// (a new buffer with no damage said: all of it)
    }
    if (surface.fresh) {
      damage = null;	// (the page has nothing of this window yet)
      surface.fresh = false;
    }
    const present = { id: ++surface.presentSeq, buffer, frames: st.frames, damage };
    if (buffer.gpu && ws.gpuSubmitted) {
      present.gpuSerial = ws.gpuSubmitted();	// (dmabuf.js: the page draws it once the GPU is past this)
    }
    surface.presents.push(present);
    ws.stats.presents++;
    if (damage === null) {
      ws.stats.damageBytes += buffer.stride * buffer.height;
    } else {
      for (const r of damage) {
        ws.stats.damageBytes += r[2] * r[3] * 4;
      }
    }
    writeSlot(surface);
    for (const child of surface.children) {
      if (child.current.buffer && child.slot < 0 && map(child)) {
        child.dirty = true;
      }
    }
  };

  // A surface that has drawn already and can be shown only now (an X window's, once the window manager has
  // matched it with its window: xwm.js): shown as it is, all of it.
  ws.showCurrent = (surface) => {
    const buffer = surface.current.buffer;
    if (!buffer || surface.slot >= 0 || !placeable(surface) || !map(surface)) {
      return;
    }
    for (const output of outputsOf(surface.object.client)) {
      send(surface.object, "enter", output);
    }
    if (ws.mapped) {
      ws.mapped(surface);
    }
    surface.fresh = false;
    surface.busy.add(buffer);
    surface.presents.push({ id: ++surface.presentSeq, buffer, frames: [], damage: null });
    ws.stats.presents++;
    writeSlot(surface);
  };

  // A rectangle of the surface (wl_surface.damage: in the surface's own coordinates) in the buffer's pixels:
  // through the viewport (wp_viewport: the destination shows the source rectangle of the buffer), then the
  // buffer's scale. A client that draws at scale 2 through a viewport (Chromium: a buffer twice the size, set
  // at buffer scale 1 with set_destination) damages in the surface's coordinates, half the buffer's pixels;
  // taken as buffer pixels only the top left quarter of each new frame would be shown, the rest left as it was.
  // Rounded outwards: a pixel partly damaged is damaged.
  const surfaceToBuffer = (surface, buffer, r) => {
    const scale = surface.current.scale;
    const vp = surface.viewport;
    let kx = 1, ky = 1, ox = 0, oy = 0;
    if (vp) {
      const src = vp.src || [0, 0, buffer.width / scale, buffer.height / scale];
      const dst = vp.dst || [src[2], src[3]];
      if (dst[0] > 0 && dst[1] > 0 && src[2] > 0 && src[3] > 0) {
        kx = src[2] / dst[0];
        ky = src[3] / dst[1];
        ox = src[0];
        oy = src[1];
      }
    }
    const x0 = Math.floor((ox + r[0] * kx) * scale), y0 = Math.floor((oy + r[1] * ky) * scale);
    const x1 = Math.ceil((ox + (r[0] + r[2]) * kx) * scale), y1 = Math.ceil((oy + (r[1] + r[3]) * ky) * scale);
    return [x0, y0, x1 - x0, y1 - y0];
  };

  const clip = (r, w, h) => {
    const x0 = Math.max(0, r[0]), y0 = Math.max(0, r[1]);
    const x1 = Math.min(w, r[0] + r[2]), y1 = Math.min(h, r[1] + r[3]);
    return [x0, y0, x1 - x0, y1 - y0];
  };

  // Does the surface have a place on the screen? A toplevel or popup has; a subsurface has when its parent
  // is shown; a surface with no role (a cursor to be) has not.
  const placeable = (surface) => {
    if (surface.xdg) {
      return ws.shellPlaceable(surface);
    }
    if (surface.sub) {
      return surface.sub.parent && surface.sub.parent.slot >= 0;
    }
    if (surface.role === "xwayland") {
      return !!(ws.xwmPlaceable && ws.xwmPlaceable(surface));	// (matched with a mapped X window: xwm.js)
    }
    if (surface.layer) {
      return ws.layerPlaceable(surface);	// (configured, not closed: layershell.js)
    }
    return surface.role === "dnd_icon" && ws.dragging && ws.dragging();	// (a drag's icon, while it lasts)
  };

  // ---- the page's acknowledgements: every turn

  // While the page is hidden (table.pageHidden(): the browser shows another tab or app, and draws none of the
  // page's frames), the server stands in for it, at the rate the page asked for: every so often each window's
  // presents count as shown, so that the frame callbacks fire and the clients go on drawing (more slowly, and
  // nobody sees it, but a program that animates or counts frames keeps running). The buffers go back to the
  // clients as well, all but the one each window shows last: that one the page has not read, and reads (all of
  // it) when it is shown again.
  let standInAt = 0;
  ws.standIns = 0;
  const standIn = () => {
    const rate = table.pageHidden();
    if (!rate) {
      return false;
    }
    const at = performance.now();
    if (at - standInAt < 1000 / rate) {
      return false;
    }
    standInAt = at;
    ws.standIns++;
    return true;
  };

  ws.pump = () => {
    const t = now();
    const standing = standIn();
    for (const surface of windows) {
      if (!surface.presents.length && !surface.dirty) {
        continue;
      }
      let acked = table.read(surface.slot);
      if (standing) {
        const last = surface.presentSeq;
        acked = { shown: Math.max(acked.shown, last), uploaded: Math.max(acked.uploaded, last - 1) };
      }
      let keep = [];
      for (const p of surface.presents) {
        if (p.id <= acked.shown) {
          for (const cb of p.frames) {
            send(cb, "done", t);
            ws.destroy(cb);
          }
          p.frames = [];
        }
        if (p.id <= acked.uploaded) {
          // The page has what it needs of this buffer: the client may draw into it again, unless a later
          // present (not uploaded yet) is of the same buffer.
          const later = surface.presents.some((q) => q.id > acked.uploaded && q.buffer === p.buffer);
          if (!later && surface.busy.has(p.buffer)) {
            surface.busy.delete(p.buffer);
            if (!p.buffer.destroyed) {
              send(p.buffer, "release");
            }
          }
        }
        if (p.id > acked.shown || p.id > acked.uploaded) {
          keep.push(p);
        }
      }
      surface.presents = keep;
      if (surface.dirty) {
        writeSlot(surface);
      }
    }
    // Surfaces the page does not show: their frame callbacks fire by the clock, so that the client goes on.
    for (const surface of allSurfaces()) {
      if (surface.waiting.length) {
        for (const cb of surface.waiting) {
          send(cb, "done", t);
          ws.destroy(cb);
        }
        surface.waiting = [];
      }
    }
  };

  const surfaces = new Set();
  const allSurfaces = () => surfaces;

  ws.busy = () => windows.some((s) => s.presents.length > 0);

  // ---- the handlers, by interface and request

  const H = {};

  H.wl_display = {
    sync(display, id) {
      const cb = ws.create(display.client, id, "wl_callback", 1);
      send(cb, "done", nextSerial());
      ws.destroy(cb);
    },
    get_registry(display, id) {
      const registry = ws.create(display.client, id, "wl_registry", 1);
      for (const g of globals) {
        if (!g.only || g.only(display.client)) {
          send(registry, "global", g.name, g.interface, g.version);
        }
      }
    },
  };

  H.wl_registry = {
    bind(registry, name, want) {
      const g = globals.find((g) => g.name === name);
      if (!g || g.interface !== want.interface || (g.only && !g.only(registry.client))) {
        ws.error(registry.client, registry, 0, "bind: no global " + name + " of " + want.interface);
        return;
      }
      if (want.version > g.version) {
        ws.error(registry.client, registry, 0, "bind: " + want.interface + " version " + want.version + " > " + g.version);
        return;
      }
      const object = ws.create(registry.client, want.id, want.interface, want.version);
      if (ws.debug) {
        log("wlserver: client " + registry.client.fd + " binds " + want.interface + " " + want.version);
      }
      g.bind(object);
    },
  };

  H.wl_compositor = {
    create_surface(compositor, id) {
      const object = ws.create(compositor.client, id, "wl_surface", compositor.version);
      const surface = newSurface(object);
      surfaces.add(surface);
      if (compositor.client.xwayland && ws.xwmNewSurface) {
        ws.xwmNewSurface(surface);	// (an X window's, maybe named already by WL_SURFACE_ID: xwm.js)
      }
      // (wl_surface 6: the scale to draw at, so that the client need not work it out from the outputs)
      send(object, "preferred_buffer_scale", screen.scale);
      send(object, "preferred_buffer_transform", 0);
      const inner = object.onDestroy;
      object.onDestroy = () => {
        inner();
        surfaces.delete(surface);
      };
    },
    create_region(compositor, id) {
      const object = ws.create(compositor.client, id, "wl_region", 1);
      object.rects = [];
    },
  };

  H.wl_region = {
    destroy() {},
    add(region, x, y, w, h) {
      region.rects.push([1, x, y, w, h]);
    },
    subtract(region, x, y, w, h) {
      region.rects.push([0, x, y, w, h]);
    },
  };

  H.wl_shm = {
    create_pool(shm, id, fd, size) {
      const object = ws.create(shm.client, id, "wl_shm_pool", shm.version);
      const pool = { fd, size, address: 0, buffers: 0, destroyed: false };
      object.pool = pool;
      if (size <= 0) {
        ws.error(shm.client, shm, 2, "create_pool: size " + size);
        return;
      }
      mapPool(pool);
      ws.stats.pools++;
      object.onDestroy = () => {
        pool.destroyed = true;
        poolDone(pool);
      };
    },
    release() {},
  };

  H.wl_shm_pool = {
    create_buffer(poolObject, id, offset, width, height, stride, format) {
      const pool = poolObject.pool;
      if (format !== FORMAT_ARGB8888 && format !== FORMAT_XRGB8888) {
        ws.error(poolObject.client, poolObject, 0, "create_buffer: format " + format + " is not offered");
        return;
      }
      if (width <= 0 || height <= 0 || stride < width * 4 || offset < 0 || offset + stride * height > pool.size) {
        ws.error(poolObject.client, poolObject, 1, "create_buffer: " + width + "x" + height + " stride " + stride
                 + " at " + offset + " does not fit the pool of " + pool.size);
        return;
      }
      const object = ws.create(poolObject.client, id, "wl_buffer", 1);
      const buffer = { pool, offset, width, height, stride, format, copy: 0, destroyed: false };
      Object.assign(object, buffer);
      object.pool = pool;
      pool.buffers++;
      object.onDestroy = () => {
        object.destroyed = true;
        pool.buffers--;
        if (object.copy) {
          ws.free(object.copy);
          object.copy = 0;
        }
        poolDone(pool);
      };
    },
    destroy() {},
    resize(poolObject, size) {
      const pool = poolObject.pool;
      if (size <= pool.size) {
        return;
      }
      // The file is bigger now; the mapping has to be made again. On this kernel a file grown after its
      // first mapping has no contiguous pages any more and cannot be mapped (fs/ramfs/file-nommu.c): then
      // the buffers of this pool are copied from the file from now on.
      unmapPool(pool);
      pool.size = size;
      mapPool(pool);
      ws.stats.resizes++;
    },
  };

  H.wl_buffer = {
    destroy() {},
  };

  H.wl_surface = {
    destroy() {},
    attach(surfaceObject, buffer, x, y) {
      const surface = surfaceObject.surface;
      surface.pending.buffer = buffer;
      if (buffer && surfaceObject.version < 5 && (x || y)) {
        surface.pending.offset = [x, y];
      }
    },
    damage(surfaceObject, x, y, w, h) {
      surfaceObject.surface.pending.damage.push([x, y, w, h]);
    },
    damage_buffer(surfaceObject, x, y, w, h) {
      surfaceObject.surface.pending.bufferDamage.push([x, y, w, h]);
    },
    frame(surfaceObject, id) {
      const cb = ws.create(surfaceObject.client, id, "wl_callback", 1);
      surfaceObject.surface.pending.frames.push(cb);
    },
    set_opaque_region(surfaceObject, region) {
      surfaceObject.surface.pending.opaque = region ? region.rects.slice() : null;
    },
    set_input_region(surfaceObject, region) {
      surfaceObject.surface.pending.input = region ? region.rects.slice() : null;
    },
    commit(surfaceObject) {
      commit(surfaceObject.surface);
    },
    set_buffer_transform(surfaceObject, transform) {
      if (transform !== 0) {
        log("compositor: buffer transform " + transform + " is not done; the picture will be as the buffer is");
      }
    },
    set_buffer_scale(surfaceObject, scale) {
      if (scale < 1) {
        ws.error(surfaceObject.client, surfaceObject, 0, "set_buffer_scale: " + scale);
        return;
      }
      surfaceObject.surface.pending.scale = scale;
    },
    offset(surfaceObject, x, y) {
      surfaceObject.surface.pending.offset = [x, y];
    },
  };

  H.wl_output = {
    release() {},
  };

  // ---- the output: one, the page's screen, of the size and scale the page says (table.js: output)

  const outputsOf = (client) => client.outputs || (client.outputs = new Set());
  const xdgOutputs = new Set();

  const describeOutput = (output) => {
    // No particular physical size: the page's pixels are what they are.
    send(output, "geometry", 0, 0, 0, 0, 0, "rvwasm", "page", 0);
    // The mode is in the output's own pixels, the screen's times its scale (wayland.xml: the buffer pixels a
    // fullscreen surface of that scale has). Sent in the page's CSS pixels, a scale-2 screen looked half its
    // size to Qt: Qt takes xdg_output's logical size only when it is not the mode's (a workaround for a
    // GNOME bug, QWaylandScreen::geometry), so it divided the mode by the scale, and every window it sizes by
    // the screen (two thirds of it, at most, for a new one) opened at a third of the screen.
    send(output, "mode", 3, screen.width * screen.scale, screen.height * screen.scale, 60000);	// (current | preferred)
    send(output, "scale", screen.scale);
  };

  // The page's screen is another size or scale now: every client is told, and the windows are fitted to it.
  // struts: the strips the page keeps for its panels at the top, left and right (the bottom one is `panel`).
  ws.setOutput = (width, height, scale, panel = 0, struts = null) => {
    width = Math.max(320, Math.min(8192, width | 0));
    height = Math.max(200, Math.min(8192, height | 0));
    scale = Math.max(1, Math.min(4, scale | 0));
    panel = Math.max(0, Math.min(height >> 2, panel | 0));
    const top = Math.max(0, Math.min(height >> 2, (struts && struts.top) | 0));
    const left = Math.max(0, Math.min(width >> 2, (struts && struts.left) | 0));
    const right = Math.max(0, Math.min(width >> 2, (struts && struts.right) | 0));
    const strutsChanged = panel !== screen.panel || top !== (screen.top || 0) || left !== (screen.left || 0) || right !== (screen.right || 0);
    screen.top = top;
    screen.left = left;
    screen.right = right;
    if (width === screen.width && height === screen.height && scale === screen.scale) {
      if (strutsChanged) {
        screen.panel = panel;	// (only the strips the page keeps: the windows are fitted, the clients need not know)
        if (ws.outputChanged) {
          ws.outputChanged();
        }
      }
      return false;
    }
    screen.panel = panel;
    const scaled = scale !== screen.scale;
    screen.width = width;
    screen.height = height;
    screen.scale = scale;
    log("compositor: the screen is " + width + "x" + height + " at scale " + scale + " now (the page keeps "
        + panel + " pixels at the bottom)");
    for (const o of xdgOutputs) {
      send(o, "logical_size", width, height);
      if (o.version < 3) {
        send(o, "done");
      }
    }
    for (const s of surfaces) {
      if (scaled && s.object.version >= 6) {
        send(s.object, "preferred_buffer_scale", scale);
      }
    }
    for (const client of ws.clients()) {
      for (const output of outputsOf(client)) {
        describeOutput(output);
        send(output, "done");
      }
    }
    if (ws.outputChanged) {
      ws.outputChanged();
    }
    return true;
  };

  H.wl_subcompositor = {
    destroy() {},
    get_subsurface(sub, id, surfaceObject, parentObject) {
      const surface = surfaceObject.surface, parent = parentObject.surface;
      if (surface.role) {
        ws.error(sub.client, sub, 0, "get_subsurface: the surface has a role already");
        return;
      }
      const object = ws.create(sub.client, id, "wl_subsurface", 1);
      surface.role = "subsurface";
      surface.sub = { object, parent, x: 0, y: 0, sync: true };
      parent.children.push(surface);
      object.surface = surface;
      object.onDestroy = () => {
        // The surface stays, without a role, and leaves the screen.
        unmap(surface);
        const at = parent.children.indexOf(surface);
        if (at >= 0) {
          parent.children.splice(at, 1);
        }
        surface.sub = null;
        surface.role = null;
      };
    },
  };

  // (Synchronized subsurfaces are not held back until the parent commits: every commit shows at once.
  // That is enough for what runs here so far; a client that relies on the atomicity will show a frame early.)
  H.wl_subsurface = {
    destroy() {},
    set_position(object, x, y) {
      const surface = object.surface;
      surface.sub.x = x;
      surface.sub.y = y;
      surface.dirty = true;
    },
    place_above(object, sibling) {
      stackChild(object.surface, sibling.surface, 1);
    },
    place_below(object, sibling) {
      stackChild(object.surface, sibling.surface, 0);
    },
    set_sync(object) {
      object.surface.sub.sync = true;
    },
    set_desync(object) {
      object.surface.sub.sync = false;
    },
  };

  const stackChild = (surface, sibling, above) => {
    if (surface.slot < 0 || sibling.slot < 0) {
      return;
    }
    const from = windows.indexOf(surface);
    windows.splice(from, 1);
    const at = windows.indexOf(sibling) + above;
    windows.splice(at, 0, surface);
    restack();
  };

  H.wp_viewporter = {
    destroy() {},
    get_viewport(viewporter, id, surfaceObject) {
      const object = ws.create(viewporter.client, id, "wp_viewport", 1);
      object.surface = surfaceObject.surface;
      object.state = { src: null, dst: null };
      object.onDestroy = () => {
        object.surface.pending.viewport = null;
      };
    },
  };

  // (A source rectangle is kept but not drawn by: the page shows the whole buffer at the destination's size.)
  H.wp_viewport = {
    destroy() {},
    set_source(vp, x, y, w, h) {
      vp.state = { src: x === -1 && w === -1 ? null : [x, y, w, h], dst: vp.state.dst };
      vp.surface.pending.viewport = vp.state.src || vp.state.dst ? vp.state : null;
    },
    set_destination(vp, w, h) {
      vp.state = { src: vp.state.src, dst: w === -1 && h === -1 ? null : [w, h] };
      vp.surface.pending.viewport = vp.state.src || vp.state.dst ? vp.state : null;
    },
  };

  H.zxdg_output_manager_v1 = {
    destroy() {},
    get_xdg_output(manager, id, output) {
      const object = ws.create(manager.client, id, "zxdg_output_v1", manager.version);
      xdgOutputs.add(object);
      object.onDestroy = () => xdgOutputs.delete(object);
      send(object, "logical_position", 0, 0);
      send(object, "logical_size", screen.width, screen.height);
      send(object, "name", "PAGE-1");
      send(object, "description", "the page the machine runs in");
      if (manager.version < 3) {
        send(object, "done");
      } else {
        send(output, "done");
      }
    },
  };

  H.zxdg_output_v1 = {
    destroy() {},
  };

  // ---- the globals offered

  global("wl_compositor", 6, () => {});
  global("wl_shm", 1, (shm) => {
    send(shm, "format", FORMAT_ARGB8888);
    send(shm, "format", FORMAT_XRGB8888);
  });
  global("wl_output", 4, (output) => {
    outputsOf(output.client).add(output);
    output.onDestroy = () => outputsOf(output.client).delete(output);
    describeOutput(output);
    send(output, "name", "PAGE-1");
    send(output, "description", "the page the machine runs in");
    send(output, "done");
  });
  global("wl_subcompositor", 1, () => {});
  global("wp_viewporter", 1, () => {});
  global("zxdg_output_manager_v1", 3, () => {});

  // ---- dispatch

  ws.dispatch = (client, object, request, args) => {
    const h = H[object.interface] && H[object.interface][request.name];
    if (h) {
      h(object, ...args);
    } else {
      log("compositor: " + object.interface + "." + request.name + " is not done (ignored)");
      // A new object the client asked for has to exist all the same, or its next request is an error.
      request.args.forEach((arg, i) => {
        if (arg[1] === "n" && arg[2] && typeof args[i] === "number") {
          ws.create(client, args[i], arg[2], object.version);
        }
      });
    }
    if (request.destructor) {
      ws.destroy(object);
    }
    return !client.gone;
  };

  ws.H = H;
  ws.bufferAddress = bufferAddress;
  ws.global = global;
  ws.globals = globals;
  ws.windows = windows;
  ws.surfaces = surfaces;
  ws.restack = restack;
  ws.unmapSurface = unmap;
  ws.mapSurface = map;
  ws.writeSlot = writeSlot;
  ws.F = { MAPPED: F_MAPPED, ACTIVATED: F_ACTIVATED, MAXIMIZED: F_MAXIMIZED, FULLSCREEN: F_FULLSCREEN,
           RESIZING: F_RESIZING, DECORATED: F_DECORATED, MINIMIZED: F_MINIMIZED };
  ws.KIND = { TOPLEVEL: KIND_TOPLEVEL, POPUP: KIND_POPUP, SUBSURFACE: KIND_SUBSURFACE, LAYER: KIND_LAYER };
})
