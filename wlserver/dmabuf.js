// Buffers that are not wl_shm pools: zwp_linux_dmabuf_v1 (version 3).
//
// A "dmabuf" here is any file the client can hand over that the server can
// map: there is no DRM driver in this machine, so what Mesa's Vulkan WSI (venus),
// zink's kopper, or an emulated render node hand over is one of the GPU
// server's backing files (gpu/server/, gpu/PROTOCOL.md), or a file in memory
// like a wl_shm pool's. Two cases, decided when the buffer is made:
//
// - The file is a GPU resource's backing: the GPU server lists every backing
//   file it made in its export table ({dev, inode} -> {resource, context}),
//   which it announces on BroadcastChannel("rv-gpu"). Then the slot says
//   format 2 (FORMAT_GPU): buffer_lo is the resource, buffer_hi the GPU server's
//   process id (resource numbers are the server's, not a context's),
//   and the page draws the executor's texture of that resource. Nothing is
//   mapped or copied. The slot's gpu_serial (offset 244) is the GPU server's
//   `submitted` word when the buffer was committed: the page draws the texture
//   once the executor's `done` has passed it.
// - Otherwise the file is mapped like a pool (one plane, LINEAR) and the slot
//   points at its pages, as for wl_shm; a file that cannot be mapped is read
//   with pread on every commit, like a grown pool.
//
// Only offered with WLSERVER_DMABUF=1 (`desktop wayland` sets it with RVGPU=1):
// a client that sees the global may prefer it to wl_shm, and nothing else here
// needs it.
//
// With WLSERVER_DMABUF_DEVICE=MAJOR:MINOR as well (`desktop` sets 226:128 whenever
// there is the GPU server: programs may get rv's emulated render node, rv/virtgpu.c), the global is
// version 4 and its feedback names that device as the one buffers are made
// on (main_device; one tranche: the four formats, LINEAR or implicit). That is how Mesa's
// EGL on Wayland finds the device to draw with: without it (or wl_drm) it
// takes its software path.
(function (ws) {
  "use strict";

  const { send, log, sys1, sys2, sys6, SYS, dv, S, envp } = ws;
  if (!envp.includes("WLSERVER_DMABUF=1")) {
    return;
  }

  const PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 1;
  const DRM_FORMAT_ARGB8888 = 0x34325241, DRM_FORMAT_XRGB8888 = 0x34325258;
  const DRM_FORMAT_ABGR8888 = 0x34324241, DRM_FORMAT_XBGR8888 = 0x34324258;
  // The slot's formats (wlserver/README.md: the slot): 0 and 1 as wl_shm's; 2 a GPU resource; 3 and 4 the
  // byte order R, G, B, A (with and without alpha), which the page draws without swizzling.
  const SLOT_FORMAT = {
    [DRM_FORMAT_ARGB8888]: 0, [DRM_FORMAT_XRGB8888]: 1, [DRM_FORMAT_ABGR8888]: 3, [DRM_FORMAT_XBGR8888]: 4,
  };
  const FORMAT_GPU = 2;
  const MOD_LINEAR_HI = 0, MOD_LINEAR_LO = 0;
  const MOD_INVALID_HI = 0x00ffffff, MOD_INVALID_LO = 0xffffffff;
  // zwp_linux_buffer_params_v1.error
  const E_ALREADY_USED = 0, E_PLANE_IDX = 1, E_PLANE_SET = 2, E_INCOMPLETE = 3, E_INVALID_FORMAT = 4,
        E_INVALID_DIMENSIONS = 5, E_OUT_OF_BOUNDS = 6;

  // ---- the GPU server's export table
  let gpu = null;	// { pid, ring, exports, cap }
  try {
    const channel = ws.channel("rv-gpu");
    channel.onmessage = (e) => {
      const m = e.data;
      if (m && m.type === "gpu" && m.version >= 2 && m.exports) {
        if (!gpu || gpu.pid !== m.pid) {
          log("dmabuf: the GPU server (pid " + m.pid + ") is there: its resources are drawn by the page");
        }
        gpu = { pid: m.pid, ring: m.ring, exports: m.exports, cap: m.exportsCap, seen: performance.now() };
      }
    };
    // The GPU server announces itself once a second; a newcomer asks, and it answers at once.
    channel.postMessage({ type: "gpu?" });
  } catch (error) {
    log("dmabuf: no BroadcastChannel (" + error.message + "): GPU resources will be mapped, not drawn by the page");
  }
  const RING_MAGIC = 0x50475652, RING_PID = 8, RING_SUBMITTED = 32, EXPORT_SIZE = 32;

  // Not heard of one yet: the file next to its socket says where it is, once it has an executor.
  const readyFile = () => {
    const e = envp.find((x) => x.startsWith("XDG_RUNTIME_DIR="));
    const text = e ? ws.readText(e.slice(16) + "/rvgpu-0.ready") : null;
    try {
      const m = text ? JSON.parse(text) : null;
      if (m && m.pid && m.ring && m.exports) {
        gpu = { pid: m.pid, ring: m.ring, exports: m.exports, cap: m.exportsCap, seen: performance.now() };
        log("dmabuf: the GPU server (pid " + m.pid + ") is there (its ready file)");
      }
    } catch (error) {
      // (being written: the announcement will come)
    }
  };

  // The resource a file is the backing of: { res, ctx, pid } or null.
  const lookup = (dev, ino) => {
    if (!gpu) {
      readyFile();
    }
    if (!gpu) {
      return null;
    }
    const v = dv();
    if (v.getUint32(gpu.ring, true) !== RING_MAGIC || v.getUint32(gpu.ring + RING_PID, true) !== gpu.pid) {
      gpu = null;	// (that server is gone)
      return null;
    }
    for (let i = 0; i < gpu.cap; i++) {
      const at = gpu.exports + 16 + EXPORT_SIZE * i;
      const res = v.getUint32(at + 12, true);
      if (res && v.getUint32(at, true) === dev >>> 0 && v.getUint32(at + 4, true) === ino % 0x100000000
          && v.getUint32(at + 8, true) === Math.floor(ino / 0x100000000)) {
        return { res, ctx: v.getUint32(at + 16, true), pid: gpu.pid };
      }
    }
    return null;
  };
  ws.gpuSubmitted = () => gpu ? dv().getUint32(gpu.ring + RING_SUBMITTED, true) : 0;

  // ---- the device buffers are made on (version 4's feedback), if there is one
  const FORMATS = [DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888];
  const deviceSetting = (envp.find((e) => e.startsWith("WLSERVER_DMABUF_DEVICE=")) || "").slice(23);
  let feedback = null;	// { device: Uint8Array (a dev_t), table: fd, tableSize }
  if (/^\d+:\d+$/.test(deviceSetting)) {
    const [major, minor] = deviceSetting.split(":").map(Number);
    // dev_t as the C library makes it (makedev()), 8 bytes little-endian.
    const dev = BigInt(((major & 0xfff) << 8) | (minor & 0xff) | ((minor & ~0xff) << 12)) | (BigInt(major & ~0xfff) << 32n);
    const device = new Uint8Array(8);
    new DataView(device.buffer).setBigUint64(0, dev, true);
    // The format table: {u32 format, u32 padding, u64 modifier} per entry, in a file the clients map. Each
    // format twice: LINEAR, and INVALID ("the driver's choice": what a driver without modifiers, Mesa's
    // virgl, must find there to make buffers at all; its buffers are linear here anyway).
    const table = new Uint8Array(16 * 2 * FORMATS.length);
    FORMATS.forEach((f, i) => {
      const v = new DataView(table.buffer);
      v.setUint32(32 * i, f, true);	// (modifier 0: LINEAR)
      v.setUint32(32 * i + 16, f, true);
      v.setUint32(32 * i + 24, MOD_INVALID_LO, true);
      v.setUint32(32 * i + 28, MOD_INVALID_HI, true);
    });
    const runtimeDir = (envp.find((e) => e.startsWith("XDG_RUNTIME_DIR=")) || "XDG_RUNTIME_DIR=/tmp").slice(16);
    const file = runtimeDir + "/.wlserver-dmabuf-formats-" + ws.sys1(SYS.getpid, 0);
    const fd = ws.sys4(SYS.openat, -100, ws.path(file, ws.P0), 2 | 0o100 | 0o1000 | 0o2000000, 0o600);
    if (fd >= 0) {
      sys2(SYS.ftruncate, fd, table.length);	// (grown from 0 once: contiguous pages, mappable shared)
      ws.writeAll(fd, table);
      ws.sys3(SYS.unlinkat, -100, ws.path(file, ws.P0), 0);
      feedback = { device, table: fd, tableSize: table.length };
      log("dmabuf: buffers are made on device " + deviceSetting + " (version 4 feedback)");
    } else {
      log("dmabuf: cannot make the format table's file (error " + -fd + "): version 3, no feedback");
    }
  }
  const sendFeedback = (fb) => {
    send(fb, "format_table", feedback.table, feedback.tableSize);
    send(fb, "main_device", feedback.device);
    send(fb, "tranche_target_device", feedback.device);
    const indices = new Uint8Array(2 * 2 * FORMATS.length);
    for (let i = 0; i < 2 * FORMATS.length; i++) {
      new DataView(indices.buffer).setUint16(2 * i, i, true);
    }
    send(fb, "tranche_formats", indices);
    send(fb, "tranche_flags", 0);
    send(fb, "tranche_done");
    send(fb, "done");
  };

  // ---- the global
  ws.global("zwp_linux_dmabuf_v1", feedback ? 4 : 3, (dmabuf) => {
    if (dmabuf.version >= 4) {
      return;	// (formats and modifiers come with the feedback from version 4 on)
    }
    for (const format of FORMATS) {
      send(dmabuf, "format", format);
      send(dmabuf, "modifier", format, MOD_LINEAR_HI, MOD_LINEAR_LO);
    }
  });

  ws.H.zwp_linux_dmabuf_feedback_v1 = {
    destroy() {},
  };

  ws.H.zwp_linux_dmabuf_v1 = {
    destroy() {},
    get_default_feedback(dmabuf, id) {
      sendFeedback(ws.create(dmabuf.client, id, "zwp_linux_dmabuf_feedback_v1", dmabuf.version));
    },
    get_surface_feedback(dmabuf, id) {
      sendFeedback(ws.create(dmabuf.client, id, "zwp_linux_dmabuf_feedback_v1", dmabuf.version));
    },
    create_params(dmabuf, id) {
      const params = ws.create(dmabuf.client, id, "zwp_linux_buffer_params_v1", dmabuf.version);
      params.planes = [];
      params.used = false;
      params.onDestroy = () => {
        for (const p of params.planes) {
          if (p && p.fd >= 0) {
            sys1(SYS.close, p.fd);
          }
        }
        params.planes = [];
      };
    },
  };

  const serverId = (client) => {
    client.nextServerId = client.nextServerId ? client.nextServerId + 1 : 0xff000000;
    return client.nextServerId;
  };

  // Make the wl_buffer (with the id given, or one of ours): the buffer object, or null after an error/failed.
  const make = (params, id, width, height, format, flags) => {
    const client = params.client;
    const fail = (code, message) => {
      if (id === null) {
        log("dmabuf: " + message + " (failed)");
        send(params, "failed");
      } else {
        ws.error(client, params, code, message);
      }
      return null;
    };
    if (params.used) {
      return fail(E_ALREADY_USED, "the params were used already");
    }
    params.used = true;
    const plane = params.planes[0];
    if (!plane || params.planes.length !== 1) {
      return fail(E_INCOMPLETE, "one plane is wanted (" + params.planes.filter(Boolean).length + " given)");
    }
    if (SLOT_FORMAT[format] === undefined) {
      return fail(E_INVALID_FORMAT, "format 0x" + format.toString(16) + " is not offered");
    }
    if (plane.modHi !== MOD_LINEAR_HI || plane.modLo !== MOD_LINEAR_LO) {
      if (!(plane.modHi === MOD_INVALID_HI && plane.modLo === MOD_INVALID_LO)) {
        return fail(E_INVALID_FORMAT, "modifier 0x" + plane.modHi.toString(16) + plane.modLo.toString(16).padStart(8, "0")
                    + " is not offered (only LINEAR)");
      }
    }
    if (width <= 0 || height <= 0 || plane.stride < width * 4) {
      return fail(E_INVALID_DIMENSIONS, width + "x" + height + " stride " + plane.stride);
    }
    if (flags) {
      return fail(E_INVALID_FORMAT, "flags " + flags + " (y-inverted, interlaced) are not done");
    }
    // What the file is.
    if (sys2(SYS.fstat, plane.fd, S) < 0) {
      return fail(E_OUT_OF_BOUNDS, "fstat of the descriptor failed");
    }
    const v = dv();
    const dev = Number(v.getBigUint64(S, true) & 0xffffffffn);
    const ino = Number(v.getBigUint64(S + 8, true));
    const fileSize = Number(v.getBigInt64(S + 48, true));
    const need = plane.offset + plane.stride * height;
    if (fileSize > 0 && need > fileSize) {
      return fail(E_OUT_OF_BOUNDS, width + "x" + height + " stride " + plane.stride + " at " + plane.offset
                  + " does not fit the file of " + fileSize + " bytes");
    }
    const object = ws.create(client, id === null ? serverId(client) : id, "wl_buffer", 1);
    const pool = { fd: plane.fd, size: Math.max(need, fileSize), address: 0, buffers: 1, destroyed: true, dmabuf: true };
    plane.fd = -1;	// (the buffer has it now)
    const found = lookup(dev, ino);
    if (ws.debug) {
      log("dmabuf: file dev " + dev + " inode " + ino + " size " + fileSize + (gpu ? " (export table at " + gpu.exports + ")" : " (no GPU server known)"));
    }
    Object.assign(object, {
      pool, offset: plane.offset, width, height, stride: plane.stride, copy: 0, destroyed: false,
      format: found ? FORMAT_GPU : SLOT_FORMAT[format], drmFormat: format,
      gpu: found ? { res: found.res, ctx: found.ctx, pid: found.pid } : null,
    });
    if (!found) {
      const r = sys6(SYS.mmap, 0, pool.size, PROT_READ | PROT_WRITE, MAP_SHARED, pool.fd, 0);
      if (r < 0 && r > -4096) {
        log("dmabuf: cannot map a buffer of " + pool.size + " bytes (error " + -r + "): it will be copied");
      } else {
        pool.address = r;
      }
    }
    if (ws.debug) {
      log("dmabuf: buffer " + object.id + " " + width + "x" + height + " "
          + (found ? "GPU resource " + found.res + " of context " + found.ctx : pool.address ? "mapped" : "copied"));
    }
    object.onDestroy = () => {
      object.destroyed = true;
      if (object.copy) {
        ws.free(object.copy);
        object.copy = 0;
      }
      if (pool.address) {
        sys2(SYS.munmap, pool.address, pool.size);
        pool.address = 0;
      }
      sys1(SYS.close, pool.fd);
      pool.fd = -1;
    };
    ws.stats.dmabufs = (ws.stats.dmabufs || 0) + 1;
    return object;
  };

  ws.H.zwp_linux_buffer_params_v1 = {
    destroy() {},
    add(params, fd, planeIdx, offset, stride, modHi, modLo) {
      if (params.used) {
        sys1(SYS.close, fd);
        ws.error(params.client, params, E_ALREADY_USED, "add after create");
        return;
      }
      if (planeIdx > 3) {
        sys1(SYS.close, fd);
        ws.error(params.client, params, E_PLANE_IDX, "plane " + planeIdx);
        return;
      }
      if (params.planes[planeIdx]) {
        sys1(SYS.close, fd);
        ws.error(params.client, params, E_PLANE_SET, "plane " + planeIdx + " twice");
        return;
      }
      params.planes[planeIdx] = { fd, offset, stride, modHi, modLo };
    },
    create(params, width, height, format, flags) {
      const buffer = make(params, null, width, height, format, flags);
      if (buffer) {
        send(params, "created", buffer);
      }
    },
    create_immed(params, id, width, height, format, flags) {
      make(params, id, width, height, format, flags);
    },
  };
})
