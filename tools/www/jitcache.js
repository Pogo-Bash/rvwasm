// The store of translated code (rv/jitcache.c says what it is for), the page's side: it is made here, handed to
// linux() (which hands it to every Worker, where __rv_jit_cache in tools/patch-runtime.py reads and adds to it),
// kept in the origin's private file system between visits, and can be started from a read-only store that was
// made when the disk image was (checked against its SHA-256 before it is used).
//
// The store is one SharedArrayBuffer, memory of the page's own: nothing on the machine can address it.
//
//   words (32 bits) 0: magic "RJC1"   1: the lock of whoever adds   2: places in the index (a power of two)
//                   3: bytes for records   4: bytes of them used   5: places used   6: records added (ever)
//                   7: layout version   8: the visit (the clock of last uses)   9: records found (this visit)
//   from byte 64:   the index, 32 bytes a place: the key (16 bytes), where the record is (4), its length (4,
//                   0: an empty place), the visit it was last found or added in (4), 4 unused. A key's first place
//                   is its first 4 bytes, as a number, modulo the places; then the next ones.
//   after it:       the records, each at a multiple of 8.
//
// While the machine runs, records are only added, never changed or taken out (they are read without a lock), and
// a store that is full takes no more. Between visits it is cut down to what was used last (compact()): when the
// page opens a store that is more than LRU_HIGH full, it keeps the records found or added in the latest visits,
// up to LRU_KEEP of its room, and forgets the rest. A store is only for the emulator that made it: its file is
// named by the emulator's SHA-256, and stores of other builds are removed (the translator's sources are in every
// key as well, see rv/jitcache.c, so an old record could never be used, only take room).
//
// Room in the browser's storage. The file is a cache: the disks' pieces (disk-worker.js) matter more, and the person's
// files most of all. So it is kept small, and it gives way:
//   - its size is decided from navigator.storage.estimate() when the page opens (budget()): a share of the quota, and
//     of what is left of it. Chrome 154 does not tell the truth about a small quota (a private window or a Guest
//     window has a few GB at most, and the estimate says usage + 10 GiB all the same: tools/storage-probe.js), so the
//     budget is also made by what happens: the disks tell on the BroadcastChannel "rvwasm-storage" when they run out of
//     room ("nospace": the file is removed, nothing more is saved) or when the browser took the files away
//     ("lost": nothing more is saved);
//   - it is written in place, only what was added since the last save (by a worker with a sync access handle, saver_main):
//     a FileSystemWritableFileStream with keepExistingData copies the whole file into a swap file first (twice the
//     room while it is open: measured, 40 MiB file, usage 40 -> 80), so that is not used;
//   - before a save the room is looked at again (room_for()): never beyond SAVE_LIMIT of the quota;
//   - any error of the file system (NotFoundError, QuotaExceededError, ...) ends the saving for the visit, with one
//     line in the log. The store itself goes on being used in memory by the machine: only the keeping stops.
"use strict";

(function (exports) {
  const MAGIC = 0x31434a52;  // "RJC1"
  const VERSION = 2;
  const HEAD = 64;
  const LRU_HIGH = 0.75;
  const LRU_KEEP = 0.5;
  const MIN_MEGABYTES = 16;        // a store with less room than this is not kept in the browser's storage
  const MEMORY_MEGABYTES = 128;    // room of a store that is not kept (it lives in the page's memory only)
  const QUOTA_SHARE = 0.10;        // of the quota the kept file may be at the most,
  const FREE_SHARE = 0.25;         // and of what is left of it
  const SAVE_LIMIT = 0.85;         // a save is not made if the storage would then be fuller than this share of the quota
  const BLOCK = 65536;             // the index is written in blocks of this many bytes, those that changed
  const CHUNK = 8 * 1048576;       // records are sent to the saver in pieces of this size

  /// An empty store: room for `megabytes` MiB of records, places for `places` of them (a power of two, at least
  /// twice as many as will be kept).
  function create(megabytes, places) {
    // (By default a place for every 2 KiB of records: a record is a region's module, some 1 to 20 KiB, or its hint,
    // under 1 KiB, one of each per region. One g++ makes some 20000 regions.)
    places = places || Math.max(65536, 2 ** Math.ceil(Math.log2(megabytes * 512)));
    const data = Math.floor(megabytes * 1048576);
    const buffer = new SharedArrayBuffer(HEAD + 32 * places + data);
    const words = new Int32Array(buffer);
    words[0] = MAGIC;
    words[2] = places;
    words[3] = data;
    words[7] = VERSION;
    return buffer;
  }

  function info(buffer) {
    const words = new Int32Array(buffer);
    return { places: words[2], capacity: words[3], used: words[4], records: words[5], added: words[6],
             visit: words[8], found: words[9] };
  }

  /// (compact(buffer, keep, megabytes, true) always makes a new store, of `megabytes` of room.)
  /// A store with only the records used most lately (by the visit they were last found or added in; the later
  /// added first among those of one visit), up to `keep` of its places and of its room: a new buffer, or the same
  /// one if nothing has to go.
  function compact(buffer, keep, megabytes, force) {
    const words = new Int32Array(buffer), bytes = new Uint8Array(buffer);
    const places = words[2], data = HEAD + 32 * places;
    // (`megabytes`: the room of the new store, if it is to have less room than this one.)
    const capacity = megabytes ? Math.min(words[3], Math.floor(megabytes * 1048576)) : words[3];
    if (!force && 2 * words[5] <= LRU_HIGH * places && words[4] <= LRU_HIGH * capacity) {
      return buffer;
    }
    const list = [];
    for (let i = 0; i < places; i++) {
      const at = (HEAD + 32 * i) >> 2;
      if (words[at + 5]) {
        list.push({ i, offset: words[at + 4], length: words[at + 5], last: words[at + 6] });
      }
    }
    list.sort((a, b) => b.last - a.last || b.offset - a.offset);
    const out = create(capacity / 1048576, places);
    const o = new Int32Array(out), ob = new Uint8Array(out);
    const room = Math.floor((keep || LRU_KEEP) * capacity), most = Math.floor((keep || LRU_KEEP) * places / 2);
    let used = 0, records = 0;
    for (const r of list) {
      const size = (r.length + 7) & ~7;
      if (records == most || used + size > room) {
        break;
      }
      let place = (words[(HEAD + 32 * r.i) >> 2]) & (places - 1);
      while (o[((HEAD + 32 * place) >> 2) + 5]) {
        place = (place + 1) & (places - 1);
      }
      const at = HEAD + 32 * place;
      ob.set(bytes.subarray(HEAD + 32 * r.i, HEAD + 32 * r.i + 16), at);
      ob.set(bytes.subarray(data + r.offset, data + r.offset + r.length), data + used);
      o[(at >> 2) + 4] = used;
      o[(at >> 2) + 5] = r.length;
      o[(at >> 2) + 6] = r.last;
      used += size;
      records++;
    }
    o[4] = used;
    o[5] = records;
    o[6] = records;
    o[8] = words[8];
    return out;
  }

  /// The head and the index of a store as they are (a copy), with `used` the bytes of records they go with. The
  /// index is copied first and `used` read after it; a place whose record is not all below `used` (one being
  /// added meanwhile) is left out of the copy.
  function head_and_index(buffer) {
    const words = new Int32Array(buffer);
    const places = words[2];
    const copy = new Uint8Array(HEAD + 32 * places);
    copy.set(new Uint8Array(buffer, 0, copy.length));
    const used = Atomics.load(words, 4);
    const w = new Int32Array(copy.buffer);
    let records = 0;
    for (let i = 0; i < places; i++) {
      const at = (HEAD + 32 * i) >> 2;
      if (w[at + 5] && w[at + 4] + w[at + 5] > used) {
        w[at + 5] = 0;
        w[at + 4] = 0;
        copy.fill(0, HEAD + 32 * i, HEAD + 32 * i + 16);
      } else if (w[at + 5]) {
        records++;
      }
    }
    w[4] = used;
    w[5] = records;
    return { copy, used };
  }

  /// What is worth saving of a store: its head, its index and the records there are (a copy, not shared).
  function snapshot(buffer) {
    const { copy: index, used } = head_and_index(buffer);
    const out = new Uint8Array(index.length + used);
    out.set(index);
    out.set(new Uint8Array(buffer, index.length, used), index.length);
    return out;
  }

  /// A store made of saved bytes (a snapshot), in a new buffer with room for `megabytes` MiB of records; null if
  /// the bytes are not a store of this layout, or have more records than there would be room for.
  function restore(bytes, megabytes) {
    bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (bytes.length < HEAD) {
      return null;
    }
    const head = new Int32Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + HEAD));
    const places = head[2], used = head[4];
    // (A file longer than that is one whose last save was cut short after the records and before the index: the
    // index is of the save before, and what comes after `used` is not looked at.)
    if (head[0] != MAGIC || head[7] != VERSION || places <= 0 || (places & (places - 1)) != 0 || used < 0 ||
        bytes.length < HEAD + 32 * places + used || used > Math.floor(megabytes * 1048576)) {
      return null;
    }
    const buffer = create(megabytes, places);
    const words = new Int32Array(buffer);
    new Uint8Array(buffer).set(bytes.subarray(HEAD, HEAD + 32 * places + used), HEAD);
    words[4] = used;
    words[5] = head[5];
    words[6] = head[6];
    words[8] = head[8];
    return buffer;
  }

  /// Is the store full (it takes no more this visit)?
  function full(buffer) {
    const i = info(buffer);
    return 2 * (i.records + 1) > i.places || i.used > 0.95 * i.capacity;
  }

  async function sha256(bytes) {
    const digest = await crypto.subtle.digest("SHA-256", bytes);
    return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
  }

  /// The read-only store that was made with a disk image: base + "jitcache.json" says {"sha256": ..., "size": ...},
  /// base + "jitcache.bin" is the store. Its bytes, if they are there and are what the description says; else null.
  async function fetch_image(base, log) {
    try {
      const described = await fetch(base + "jitcache.json", { cache: "no-cache" });
      if (!described.ok) {
        return null;
      }
      const want = await described.json();
      const response = await fetch(base + "jitcache.bin");
      if (!response.ok) {
        return null;
      }
      const bytes = new Uint8Array(await response.arrayBuffer());
      const got = await sha256(bytes);
      if (bytes.length != want.size || got != want.sha256) {
        log("[JIT cache]: the disk's store of translations is not what its description says (" + got + "): not used");
        return null;
      }
      return bytes;
    } catch (error) {
      log("[JIT cache]: no store of translations with the disk (" + error.message + ")");
      return null;
    }
  }

  /// A store as it is kept, made ready for a new visit: the next visit on the clock, cut down to what was used
  /// lately if it is fuller than LRU_HIGH (compact()). Returns { buffer, compacted }.
  function next_visit(buffer) {
    const before = info(buffer).records;
    const out = compact(buffer);
    const words = new Int32Array(out);
    words[8]++;
    words[9] = 0;
    return { buffer: out, compacted: out != buffer ? before - info(out).records : 0 };
  }

  /// What navigator.storage.estimate() says, { usage, quota } in bytes, or null.
  async function storage_estimate() {
    try {
      const e = await navigator.storage.estimate();
      return e && e.quota > 0 ? { usage: e.usage || 0, quota: e.quota } : null;
    } catch (error) {
      return null;
    }
  }

  /// MiB of room in the browser's storage for the kept file, of `wanted`: at most QUOTA_SHARE of the quota and FREE_SHARE of
  /// what is left of it (`own`: bytes of the file already there, not counted against it). 0 if that is less than
  /// MIN_MEGABYTES, and the store is then not kept. Without an estimate, `wanted`.
  function budget(wanted, estimate, own) {
    if (!estimate) {
      return wanted;
    }
    const others = Math.max(0, estimate.usage - (own || 0));
    const left = Math.max(0, estimate.quota - others);
    const mb = Math.floor(Math.min(wanted, QUOTA_SHARE * estimate.quota / 1048576, FREE_SHARE * left / 1048576));
    return mb < MIN_MEGABYTES ? 0 : mb;
  }

  /// Is there room to write `need` more bytes and not be near the quota? (Looked at before every save.)
  async function room_for(need) {
    const e = await storage_estimate();
    return !e || e.usage + need <= SAVE_LIMIT * e.quota;
  }

  /// Blocks of `next` that are not the same in `before` (or all of it, without `before`), as { at, data } (copies).
  function changed_blocks(before, next) {
    const out = [];
    for (let at = 0; at < next.length; at += BLOCK) {
      const end = Math.min(next.length, at + BLOCK);
      let same = !!before && before.length >= end;
      if (same) {
        // (Words first, they are what the index is made of; the length is a multiple of 8 but for the tail.)
        const a = new Uint8Array(before.buffer, before.byteOffset + at, end - at), b = new Uint8Array(next.buffer, next.byteOffset + at, end - at);
        for (let i = 0; i < a.length; i++) {
          if (a[i] != b[i]) {
            same = false;
            break;
          }
        }
      }
      if (!same) {
        out.push({ at, data: next.slice(at, end) });
      }
    }
    return out;
  }

  /// The worker that writes the file (this function's text is its script): a FileSystemSyncAccessHandle writes in place,
  /// where a writable stream would copy the whole file first. One message in, one answer out ({ id, ... } or { id, error }).
  function saver_main() {
    let handle = null, root = null, file_name = "";
    const fail = (code) => ({ name: code == -4 ? "NotFoundError" : code == -8 ? "QuotaExceededError" : "FileError",
                              message: "file error " + code });
    const count = (n) => { if (n > 0x7fffffff) { throw Object.assign(new Error(fail(n | 0).message), { name: fail(n | 0).name }); } return n; };
    onmessage = async (event) => {
      const m = event.data;
      try {
        if (m.op == "open") {
          root = await navigator.storage.getDirectory();
          file_name = m.name;
          const file = await root.getFileHandle(m.name, { create: true });
          for (let attempt = 0; ; attempt++) {
            try {
              handle = await file.createSyncAccessHandle();
              break;
            } catch (error) {
              // (A page just reloaded: the browser lets go of the file a little after the worker that had it is gone.)
              if (error.name != "NoModificationAllowedError" || attempt == 40) {
                throw error;
              }
              await new Promise((resolve) => setTimeout(resolve, 50));
            }
          }
          postMessage({ id: m.id, size: handle.getSize() });
        } else if (m.op == "write") {
          for (const w of m.writes) {
            if (count(handle.write(w.data, { at: w.at })) != w.data.length) {
              throw Object.assign(new Error("short write"), { name: "QuotaExceededError" });
            }
          }
          if (m.size !== undefined) {
            handle.truncate(m.size);
          }
          handle.flush();
          postMessage({ id: m.id, size: handle.getSize() });
        } else if (m.op == "remove") {
          if (handle) {
            handle.close();
            handle = null;
          }
          root = root || await navigator.storage.getDirectory();
          await root.removeEntry(m.name);
          postMessage({ id: m.id });
        } else if (m.op == "close") {
          if (handle) {
            handle.close();
            handle = null;
          }
          postMessage({ id: m.id });
        }
      } catch (error) {
        postMessage({ id: m.id, error: { name: error.name, message: error.message } });
      }
    };
  }

  /// For the page: the store to hand to linux(), from the origin's private file system if it was kept there (as
  /// "jitcache-" + the emulator's SHA-256 + ".bin"; the stores of other builds of it are removed), else from the
  /// disk image's (base, see fetch_image()), else empty; and from then on saved there every minute when something
  /// was added, and when the page is hidden. options: { megabytes, program (the SHA-256), base, reset, log }.
  /// How much room in the browser's storage it may take is decided here (budget()), and it stops being kept at
  /// the first sign that the storage is short or gone (see the head of this file); then it lives in memory.
  async function open(options) {
    const log = options.log || (() => {});
    const wanted = options.megabytes || 128;
    let buffer = null, from = "empty";
    let dir = null;
    try {
      dir = await navigator.storage.getDirectory();
    } catch (error) {
      log("[JIT cache]: no private file system (" + error.message + "): kept for this visit only");
    }
    const name = "jitcache-" + options.program.slice(0, 16) + ".bin";
    if (dir) {
      try {
        for await (const [entry] of dir.entries()) {
          if (entry.startsWith("jitcache-") && (entry != name || options.reset)) {
            await dir.removeEntry(entry);
            log("[JIT cache]: removed " + entry + (entry == name ? "" : " (another build of the emulator)"));
          }
        }
      } catch (error) {
        // (Nothing to remove, or it cannot be: it only takes room.)
      }
    }
    // The file from before, its bytes (if it is there); the room there is for it in the storage.
    let kept_bytes = null;
    if (dir && !options.reset) {
      try {
        const file = await (await dir.getFileHandle(name)).getFile();
        kept_bytes = new Uint8Array(await file.arrayBuffer());
      } catch (error) {
        // (None kept yet.)
      }
    }
    const estimate = dir ? await storage_estimate() : null;
    let megabytes = dir ? budget(wanted, estimate, kept_bytes ? kept_bytes.length : 0) : wanted;
    let persist = !!dir && megabytes > 0;
    if (dir && !persist) {
      megabytes = Math.min(wanted, MEMORY_MEGABYTES);
      log("[JIT cache]: this browser's storage is small (" + (estimate ? Math.round(estimate.quota / 1048576) + " MiB, "
          + Math.round(estimate.usage / 1048576) + " in use" : "unknown") + "): kept for this visit only, " + megabytes + " MiB of room");
    } else if (megabytes < wanted) {
      log("[JIT cache]: room for " + megabytes + " MiB of the " + wanted + " asked for, by the storage there is (" +
          Math.round(estimate.quota / 1048576) + " MiB, " + Math.round(estimate.usage / 1048576) + " in use)");
    }
    let compacted = 0;
    if (kept_bytes && persist) {
      // (A file made with more room than there is now is cut down to what fits: the records used lately.)
      const head = new Int32Array(kept_bytes.buffer.slice(kept_bytes.byteOffset, kept_bytes.byteOffset + Math.min(HEAD, kept_bytes.length)));
      const had = head.length >= 4 ? Math.max(megabytes, Math.ceil(head[3] / 1048576)) : megabytes;
      let restored = restore(kept_bytes, had);
      if (restored) {
        if (had > megabytes) {
          const before = info(restored).records;
          restored = compact(restored, LRU_KEEP, megabytes, true);
          compacted = before - info(restored).records;
          buffer = restored;
          from = "kept (cut down to " + megabytes + " MiB, " + compacted + " records forgotten)";
        } else {
          let more;
          ({ buffer, compacted: more } = next_visit(restored));
          from = more ? "kept (" + more + " records used least lately forgotten)" : "kept";
        }
      }
    }
    // The head and index as the file has them, when the store is the file's as it was (not cut down): so that the first
    // save writes only the blocks of the index that changed.
    const file_index = from == "kept" ? kept_bytes.slice(0, HEAD + 32 * info(buffer).places) : null;
    kept_bytes = null;
    if (!buffer && options.base) {
      const image = await fetch_image(options.base, log);
      if (image) {
        buffer = restore(image, megabytes);
        if (buffer) {
          buffer = next_visit(buffer).buffer;
          from = "the disk's";
        }
      }
    }
    if (!buffer) {
      buffer = create(megabytes);
    }
    const i = info(buffer);
    log("[JIT cache]: " + from + ", " + i.records + " records, " + (i.used >> 20) + " of " + (i.capacity >> 20) + " MiB");

    if (persist) {
      let saver = null, ended = false;
      let next_id = 1;
      const waiting = new Map();
      // What the file has: everything below `saved_used` of the records is there already (records are never changed),
      // and `saved_index` is the head and index as they were written, so that a save writes the blocks of the index that
      // changed and the records added since, not the whole store again.
      // (A store cut down, or the disk's, is written whole the first time.)
      let saved = info(buffer).added, saved_used = from == "kept" ? info(buffer).used : 0, saved_found = 0;
      let saved_index = file_index, saving = false, timer = 0;
      const ask = (message, transfer) => new Promise((resolve, reject) => {
        const id = next_id++;
        waiting.set(id, { resolve, reject });
        saver.postMessage(Object.assign({ id }, message), transfer || []);
      });
      const stop = async (why, remove) => {
        if (ended) {
          return;
        }
        ended = true;
        clearInterval(timer);
        // One line, once. (The store goes on in memory: only the keeping stops.)
        log("[JIT cache]: not kept in the browser's storage any more (" + why + "); it stays in memory for this visit");
        try {
          if (saver && remove) {
            await ask({ op: "remove", name });
          } else if (saver) {
            await ask({ op: "close" });
          }
        } catch (error) {
          // (Gone, or cannot be: nothing more to do.)
        }
        if (saver) {
          saver.terminate();
          saver = null;
        }
        for (const [, w] of waiting) {
          w.reject(new Error("stopped"));
        }
        waiting.clear();
      };
      try {
        const blob = new Blob(["(" + saver_main.toString() + ")()"], { type: "text/javascript" });
        const url = URL.createObjectURL(blob);
        saver = new Worker(url, { name: "jitcache-saver" });
        URL.revokeObjectURL(url);
        saver.onmessage = (event) => {
          const w = waiting.get(event.data.id);
          if (w) {
            waiting.delete(event.data.id);
            event.data.error ? w.reject(Object.assign(new Error(event.data.error.message), { name: event.data.error.name })) : w.resolve(event.data);
          }
        };
        saver.onerror = (event) => {
          event.preventDefault && event.preventDefault();
          stop("the saving worker failed" + (event.message ? ": " + event.message : ""), false);
        };
      } catch (error) {
        persist = false;
        saver = null;
        log("[JIT cache]: cannot start the saving worker (" + error.message + "): kept for this visit only");
      }
      if (saver) {
        let opened = false;
        const save = async () => {
          if (saving || ended) {
            return;
          }
          const now = info(buffer);
          // (What was found this visit is saved too: when each record was last used is in the index.)
          if (now.added == saved && now.found == saved_found) {
            return;
          }
          saving = true;
          try {
            const { copy: index, used } = head_and_index(buffer);
            const index_writes = changed_blocks(saved_index, index);
            const need = (used - saved_used) + index_writes.reduce((n, w) => n + w.data.length, 0);
            if (!await room_for(need)) {
              await stop("the storage is nearly full", true);
              return;
            }
            if (!opened) {
              await ask({ op: "open", name });
              opened = true;
              if (saved_used == 0) {
                // A store written whole, over a file that may have another: emptied first, so that a save cut short leaves
                // a file that is not a store (restore() looks for the magic number in its head), not a mix of two.
                await ask({ op: "write", writes: [], size: 0 });
              }
            }
            // The records first, the head and the index after them: a save that is cut short leaves a file whose index
            // is the one before (restore() takes a file that is longer than its index says).
            for (let at = saved_used; at < used; at += CHUNK) {
              const end = Math.min(used, at + CHUNK);
              const data = new Uint8Array(end - at);
              data.set(new Uint8Array(buffer, index.length + at, end - at));
              await ask({ op: "write", writes: [{ at: index.length + at, data }] }, [data.buffer]);
              if (ended) {
                return;
              }
            }
            await ask({ op: "write", writes: index_writes, size: index.length + used }, index_writes.map((w) => w.data.buffer));
            log("[JIT cache]: saved, " + info(buffer).records + " records, " + (used >> 20) + " MiB (" +
                Math.round(need / 1048576 * 10) / 10 + " MiB written); " + info(buffer).found + " found this visit");
            saved = now.added;
            saved_found = now.found;
            saved_used = used;
            saved_index = index;
          } catch (error) {
            // NotFoundError (the storage was taken away), QuotaExceededError, anything: no more tries this visit.
            await stop(error.name + (error.name == "NotFoundError" ? ": the browser removed the site's storage" : ": " + error.message),
                       error.name == "QuotaExceededError");
          }
          saving = false;
        };
        timer = setInterval(save, 60000);
        document.addEventListener("visibilitychange", () => {
          if (document.visibilityState == "hidden") {
            save();
          }
        });
        exports.save_now = save;
        // The disks tell what they see (disk-worker.js): the storage is gone, or full.
        if (typeof BroadcastChannel == "function") {
          const channel = new BroadcastChannel("rvwasm-storage");
          channel.onmessage = (event) => {
            const m = event.data || {};
            if (m.type == "lost") {
              stop("the browser removed the site's storage", false);
            } else if (m.type == "nospace") {
              stop("a disk has no room left in the storage", true);
            }
          };
        }
      }
    }
    return buffer;
  }

  Object.assign(exports, { create, info, snapshot, head_and_index, restore, full, compact, next_visit, open, fetch_image,
                           sha256, budget, changed_blocks, room_for, storage_estimate });
})(typeof module != "undefined" ? module.exports : (globalThis.jitcache = {}));
