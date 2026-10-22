// A disk of the Linux/Wasm system (drivers/block/wasm_blk.c), kept by a
// worker of its own (one per disk).
//
// The disk is an image on the server, cut into pieces (tools/split-disk.py).
// A piece is fetched the first time something in it is read or written and
// from then on lives here: in two files in the origin's private file system
// (OPFS), so that it is still here, with everything the system wrote, the
// next time the page is loaded.
//
//   data   the pieces that are here, one after the other in the order they
//          arrived (not a file as large as the disk with holes in it: Chrome
//          counts a file's whole length against the origin's quota)
//   map    a heading that names the image, then one 32-bit number per piece
//          of the disk: its place in `data` plus one, or 0 if it is not here
//
// Why a worker of its own: a file in OPFS is read and written without
// waiting only through a FileSystemSyncAccessHandle, which exists only in
// dedicated workers, cannot be handed from one worker to another, and takes
// an asynchronous call to open, which a kernel task that is in the middle of
// its Wasm code cannot make. So this worker opens the files once; a task
// that needs sectors puts its request into shared memory, wakes this worker
// and sleeps until the answer is there (see wasm_driver_blk_rw in
// linux-worker.js). The sectors go straight between the file and the
// machine's memory: read() and write() take views of shared memory.
//
// A disk that is somebody's own (message.keep: the one a person's files are
// on) starts from an image too, an empty file system, but only once: the
// first time, all of that image is fetched, and from then on the disk is
// what is here and the server is not asked again, whatever image it has by
// then. So the system's image can be replaced without touching that disk.
//
// Fetching ahead (the pool). A kernel task that waits for a piece fetches it with one synchronous request, and
// one piece at a time is slow (~0.15-0.3 s each, 3-6 MB/s): a cold boot reads some 700 pieces. So a second worker of
// this worker's own, the fetcher (this same file, named "disk-fetcher"), fetches pieces nobody is waiting for yet,
// asynchronously, LIMIT at a time (6, or ?fetchers=N on the page's address; 0 for none), from the object storage
// only (never the site: its pieces are rate limited per address, see docs/deploy.md), and hands them over in a few
// slots of shared memory (SLOTS pieces, 24 MiB at the most, whatever is fetched). What is fetched ahead:
//   - the manifest's `boot` list: the pieces a cold boot to the desktop reads, in the order it reads them
//     (tools/boot-list.py; r2-publish.py puts it in the manifest, which is not part of the disk's `id`);
//   - read-ahead on runs of consecutive pieces: 4 ahead at the second piece of a run, then 8, then 16.
// The order is: pieces a copy is waiting for (the home disk's first start), then read-ahead, then the boot list. A piece
// the kernel is waiting for never queues: if the fetcher has it in flight this worker waits for that request (a
// piece is asked for once), if it is still in the queue this worker takes it out and fetches it at once with its
// own synchronous request, as it always did; and while that request runs the fetcher keeps one fewer request of its
// own going, so that the pool and the piece being waited for together stay within LIMIT. Pieces the fetcher has
// brought are written into `data` by this worker when it has nothing else to do (one piece between two looks at
// the request words), so the files are written as slowly as they are fetched and never more than SLOTS pieces are
// waiting. Both workers share one table of piece states (STATE_*), changed only by compare-and-exchange. Nothing
// here is needed: if the fetcher cannot be started, or the files are not OPFS ones (memory would grow with every
// piece fetched ahead), or the manifest has no object storage, everything is fetched on demand as before.
//
// The files are opened in the ordinary way, which locks them: a second tab
// with the same disk cannot have them, and neither can this worker if OPFS
// is missing or refuses. Then the pieces are kept in memory instead and are
// gone when the page is: the system works, and forgets.
//
// When the browser takes the files away (storage_lost). A browser can remove an origin's private file system while
// a page is using it: the person can clear the site's data, and a Guest or private window's storage is small (Chrome
// gives such a profile a share of the computer's memory, keeps its files in memory and treats them as best effort,
// docs/storage.md). The open handles then answer every read and write with an error code: Chrome returns -4
// (base::File's FILE_ERROR_NOT_FOUND) as an unsigned 32-bit number, 4294967292, and getSize() 2^64 - 4. This worker
// takes that (and a NotFoundError or InvalidStateError from any of its calls) for "the files are gone", once, whether
// a request finds it out or the look it takes every PROBE_EVERY ms while idle:
//   - the disk goes on from memory, and says so on the BroadcastChannel "rvwasm-storage" (the page shows a note);
//   - a disk of the server's image (not `keep`): every piece that was only fetched is fetched again when it is next
//     wanted (the site's pieces are content addressed and kept by the server), so the system goes on as it was; a
//     piece the machine had written to is the machine's own: those that this visit wrote to are copies kept in memory
//     from the first write on (`mirror`, up to MIRROR_BASE bytes) and go on from there, the others are what the
//     image has;
//   - a disk that is somebody's own (`keep`): nothing of it can be fetched, so every piece that is touched is also
//     kept in memory (`mirror`, up to MIRROR_KEEP bytes, all of it at the start if the disk is small) and after the
//     loss the disk is those copies. What was beyond that is zeros;
//   - the pieces that are only in memory and can be fetched again (clean pieces of a disk of the image) are kept
//     up to MEMORY_CLEAN of them, the least lately used go: memory is not the quota's replacement;
//   - nothing is written to the browser's storage again by this page: whatever took it away would only do so again.
//
// What is kept when: a write lands in `data` at once, but the browser and
// the operating system may hold it back. `map` is only written after `data`
// has been flushed, so that it never points at something that is not there:
// when the kernel asks for a flush (a file system does, when it must be sure),
// after a second without requests, and every 64 new pieces.

"use strict";

const SECTOR = 512;
const HEADING = 64;

// The shared words of a request; the same numbers are in linux-worker.js.
// (The address in two words: the machine's memory may be larger than 4 GiB. A runtime from before that leaves
// ADDRESS_HIGH 0, which is right for the memory it makes.)
const LOCK = 0, REQUEST = 1, DONE = 2, OP = 3, SECTOR_LOW = 4, SECTOR_HIGH = 5, ADDRESS = 6, COUNT = 7, RESULT = 8,
      SECTORS_LOW = 9, SECTORS_HIGH = 10, ADDRESS_HIGH = 11;
const OP_READ = 0, OP_WRITE = 1, OP_FLUSH = 2;
const EIO = 5;

let memory = null;      // the machine's memory
let control = null;     // Int32Array: the shared words above
let base = "";          // where the site keeps the pieces (and the manifest)
let remote = "";        // where the manifest says the same pieces are, by hash (object storage), or ""
let hashes = null;      // per piece, its sha256 as the manifest says, "" for none
let remote_failures = 0;
let remote_off_until = 0;
let size = 0;           // bytes in the disk
let piece_size = 0;
let present = null;     // Uint8Array, per piece: 1 if the server has a file for it, 0 if it is all zeros
let place = null;       // Uint32Array, per piece: its place in `data` plus one, or 0
let places = 0;         // places in use in `data`
let data = null;        // FileSystemSyncAccessHandle, or null: nothing is kept
let map = null;
let unsaved = 0;        // pieces placed since `map` was last written
const in_memory = new Map();  // piece -> Uint8Array, for the pieces that are not in `data`
let direct = true;      // whether read() and write() take views of the machine's (shared) memory
let complained = false;
let disk_name = "";
let keep_disk = false;  // the disk is somebody's own (message.keep)
let lost = false;       // the browser took the files away (storage_lost)
let saved_at = 0;       // when `map` was last written (or the files opened), ms since the epoch
const dirty = new Set();   // pieces the machine wrote to in this visit
const mirror = new Map();  // piece -> Uint8Array: a copy in memory of a piece that is in `data` (see "When the browser takes the files away")
const MIRROR_KEEP = 256 * 1048576, MIRROR_BASE = 128 * 1048576;  // bytes of copies, for a disk that is kept / one of the image
const PROBE_EVERY = 5000;  // ms between looks at whether the files are still there, while the disk is idle
let probed_at = 0;
const MEMORY_CLEAN = 160;  // pieces of a disk of the image that are only in memory and clean (fetched again if wanted) at the most
const channel = typeof BroadcastChannel == "function" ? new BroadcastChannel("rvwasm-storage") : null;
let pool = null;        // the tables shared with the fetcher (see "Fetching ahead"), or null: all is fetched here, as it always was
let pool_worker = null; // the fetcher, from its start until the tables are ready
let serving = false;    // whether the machine is running (a request is waited for): demand against the pool counts from then on
let traced = null;      // Uint8Array, per piece: whether the trace has said it
let how = "";           // how the piece that `bring` last had got here, for the trace

const log = (text) => postMessage({ method: "log", text: text });

/// A line for a problem that may come again and again (a failed request, once per request): the first one, then
/// every tenth time it has come, with the count, so that a log is not filled by one thing going wrong 4096 bytes
/// at a time. `key` says which problem it is.
const repeats = new Map();
const log_limited = (key, text) => {
  const n = (repeats.get(key) || 0) + 1;
  repeats.set(key, n);
  if (n == 1) {
    log(text);
  } else if (n == 10 || n == 100 || n % 1000 == 0) {
    log(text + " (" + n + " times)");
  }
};

// ---- the pool: shared by both workers ----
const SLOTS = 24;               // pieces of shared memory between the fetcher and this worker (24 MiB)
const FETCHERS_DEFAULT = 6, FETCHERS_MAX = 16;
const AHEAD_FIRST = 4, AHEAD_MAX = 16;   // read-ahead: pieces asked for at the second piece of a run, and the most
const WAIT_FLYING = 20000;      // ms a piece the fetcher has in flight is waited for before it is fetched here too (?flying=ms)
const FETCH_TIMEOUT = 30000;    // ms for one of the fetcher's requests
// A piece's state (the `state` table, one word per piece): who may change it from what is in the comments.
const ST_IDLE = 0;      // nobody has it, nobody asked for it
const ST_QUEUED = 1;    // in the fetcher's queue (this worker: IDLE -> QUEUED; the fetcher: QUEUED -> FLYING)
const ST_FLYING = 2;    // the fetcher is fetching it (-> READY when the bytes are in a slot, -> FAILED, -> QUEUED to try again)
const ST_READY = 3;     // in a slot (this worker takes it: -> HAVE)
const ST_HAVE = 5;      // in `data` or in memory here (or all zeros, or never to be fetched ahead)
const ST_MINE = 6;      // this worker is fetching it itself (the fetcher leaves it alone; from QUEUED, IDLE, FAILED or FLYING after WAIT_FLYING)
const ST_FAILED = 7;    // the fetcher could not get it: only this worker's own request is left
// What a piece is queued for, first first.
const KIND_COPY = 0, KIND_AHEAD = 1, KIND_BOOT = 2;
// The shared flags.
const FL_DEMAND = 0;    // requests of this worker's own that are running (counted against the fetcher's limit)
const FL_OFF = 1;       // ms since the start before which the object storage is left alone (this worker sets it after a refusal)
const FL_STOP = 2;      // 1: stop fetching ahead (the quota is out)

/// A number in the address of this worker's script (?fetchers=6), or `fallback`.
const setting = (name, fallback) => {
  const found = new RegExp("[?&]" + name + "=(\\d+)").exec((self.location && self.location.search) || "");
  return found ? Number(found[1]) : fallback;
};
const limit = Math.min(FETCHERS_MAX, setting("fetchers", FETCHERS_DEFAULT));
const wait_flying = setting("flying", WAIT_FLYING);
const is_fetcher = self.name == "disk-fetcher";

// Opt-in trace of the disk's demand (`disk-worker.js?...&trace`, or DISK_TRACE set by tools/node-machine.js): one log
// line "trace <ms since start> <disk> <piece> <fetch ms or -1 for kept> <demand|ahead|boot>" for every piece a read or
// write touches the first time. tools/boot-trace.py turns a cold boot's lines into the boot prefetch list.
const trace_on = !!globalThis.DISK_TRACE || /[?&]trace\b/.test((self.location && self.location.search) || "");
const started_at = Date.now();

// How hard a piece is asked for. A refusal is not an I/O error until this has run out: the guest simply waits (the
// request is synchronous), so a busy server (a 429 or 503 from the site's rate limit) costs a slow read, not a broken
// file system. Waits are never shorter than the server asks and never hammer: they double from RETRY_FIRST to
// RETRY_MAX with jitter, and a Retry-After is honoured up to RETRY_MAX.
const RETRY_FIRST = 500;       // ms, the first wait after an error that comes without a Retry-After
const RETRY_MAX = 30000;       // ms, the longest single wait (also the longest Retry-After that is believed)
const RETRY_TOTAL = 120000;    // ms, how long one piece is tried before the read fails
const FATAL_TRIES = 2;         // a 404 and the like are not going to get better: this many tries, then the error

/// Wait `ms` without leaving the worker (the request in progress is synchronous, so nothing else is waiting on this
/// thread). A private word that nobody wakes: the wait just times out.
const sleep = (ms) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);

/// A Retry-After header (seconds, or a date) as milliseconds, or -1 if there is none that makes sense.
const retry_after = (value) => {
  if (!value) {
    return -1;
  }
  value = String(value).trim();
  const ms = /^\d+$/.test(value) ? Number(value) * 1000 : Date.parse(value) - Date.now();
  return Number.isFinite(ms) ? Math.max(0, Math.min(RETRY_MAX, ms)) : -1;
};

/// The fetcher (the worker named "disk-fetcher", started by the disk worker): a queue of pieces by kind, at most
/// `limit` requests to the object storage at a time (minus the ones the disk worker has running for a task that waits),
/// the answers into slots of shared memory. It never serves the site, never writes the disk, and the disk worker
/// does not depend on it: it can be late, or dead.
const fetcher_main = () => {
  let up = null;                 // the disk worker's "init": the tables and what is needed to name a piece
  const queues = [[], [], []];
  const heads = [0, 0, 0];
  const kinds = new Map();       // piece -> kind, for a piece that is put back
  const tries = new Map();       // piece -> requests that were refused
  let flying = 0;
  let paused_until = 0;          // ms since up.started_at: nothing is started before (a Retry-After, or the storage failing)
  let failures = 0;              // requests in a row that failed
  let timer = 0;
  const loading = [];            // per slot: a request is filling it
  const clock = () => Date.now() - up.started_at;

  const free_slot = () => {
    for (let slot = 0; slot < SLOTS; slot++) {
      if (!loading[slot] && Atomics.load(up.slot_piece, slot) == 0) {
        return slot;
      }
    }
    return -1;
  };
  const next_piece = () => {
    for (let kind = 0; kind < queues.length; kind++) {
      const queue = queues[kind];
      while (heads[kind] < queue.length) {
        const piece = queue[heads[kind]++];
        if (Atomics.load(up.state, piece) == ST_QUEUED) {
          return piece;
        }
      }
      queue.length = 0;      // (all gone: forget them, a queue of 700 is not worth keeping)
      heads[kind] = 0;
    }
    return -1;
  };

  /// Start as many requests as the limit, the free slots and the pauses allow.
  const pump = () => {
    if (!up || Atomics.load(up.flags, FL_STOP)) {
      return;
    }
    for (;;) {
      if (flying + Atomics.load(up.flags, FL_DEMAND) >= up.limit) {
        return;
      }
      const wait = Math.max(paused_until, Atomics.load(up.flags, FL_OFF)) - clock();
      if (wait > 0) {
        clearTimeout(timer);
        timer = setTimeout(pump, wait + 5);
        return;
      }
      const slot = free_slot();
      if (slot < 0) {
        return;              // (the disk worker says when one is free again: "kick")
      }
      const piece = next_piece();
      if (piece < 0) {
        return;
      }
      if (Atomics.compareExchange(up.state, piece, ST_QUEUED, ST_FLYING) != ST_QUEUED) {
        continue;            // (the disk worker took it in the meantime)
      }
      flying++;
      loading[slot] = true;
      run(piece, slot);
    }
  };

  const run = async (piece, slot) => {
    const expected = Math.min(up.piece_size, up.size - piece * up.piece_size);
    let done = false;
    let problem = "";
    let wait = -1;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), FETCH_TIMEOUT);
    try {
      const response = await fetch(up.remote + up.hashes[piece] + ".bin", { signal: controller.signal });
      if (response.status == 200) {
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (bytes.byteLength == expected) {
          const view = new Uint8Array(up.slots, slot * up.piece_size, up.piece_size);
          view.set(bytes);
          view.fill(0, bytes.byteLength);
          done = true;
        } else {
          problem = bytes.byteLength + " bytes";
        }
      } else {
        problem = "status " + response.status;
        if (response.status == 429 || response.status == 503) {
          // (A Retry-After the browser does not let the page read, from another origin, is a pause of five seconds.)
          wait = retry_after(response.headers.get("Retry-After"));
          if (wait < 0) {
            wait = 5000;
          }
        }
      }
    } catch (error) {
      problem = error.name || "error";
    }
    clearTimeout(timeout);
    flying--;
    loading[slot] = false;
    if (done) {
      failures = 0;
      Atomics.store(up.slot_of, piece, slot);
      Atomics.store(up.slot_piece, slot, piece + 1);
      if (Atomics.compareExchange(up.state, piece, ST_FLYING, ST_READY) == ST_FLYING) {
        Atomics.notify(up.state, piece);
        Atomics.notify(up.control, REQUEST);      // (the disk worker looks at what is ready when it wakes)
      } else {
        Atomics.store(up.slot_piece, slot, 0);    // (the disk worker did not wait for it and has its own)
      }
    } else {
      // A refusal that says when to come back (Retry-After) stops the pool until then and the piece goes back in the
      // queue once; anything else is one try, and the piece is left to the disk worker's own request.
      const now = clock();
      if (wait >= 0) {
        paused_until = Math.max(paused_until, now + Math.max(wait, 1000));
      }
      const again = (tries.get(piece) || 0) < 1 && (wait >= 0 || !/^status 4/.test(problem));  // (not a 403 or a 404: it will say the same)
      tries.set(piece, (tries.get(piece) || 0) + 1);
      if (again) {
        // (the front of its kind)
        const kind = kinds.get(piece) ?? KIND_BOOT;
        queues[kind].splice(heads[kind], 0, piece);
        if (Atomics.compareExchange(up.state, piece, ST_FLYING, ST_QUEUED) == ST_FLYING) {
          Atomics.notify(up.state, piece);
        }
      } else if (Atomics.compareExchange(up.state, piece, ST_FLYING, ST_FAILED) == ST_FLYING) {
        Atomics.notify(up.state, piece);
      }
      if (wait < 0 && ++failures >= 3) {
        // The storage is not answering: not for five minutes (the disk worker's own requests go to the site then, as
        // they do when they fail three times in a row).
        paused_until = Math.max(paused_until, now + 300000);
        failures = 0;
      }
    }
    pump();
  };

  onmessage = (event) => {
    const message = event.data;
    try {
      if (message.op == "init") {
        up = message;
        postMessage({ method: "up" });
      } else if (!up) {
        return;
      } else if (message.op == "enqueue") {
        for (const piece of message.pieces) {
          queues[message.kind].push(piece);
          kinds.set(piece, message.kind);
        }
      } else if (message.op == "promote") {
        // (a piece that is queued for a lesser kind: it is also in the queue of this one, and the first to be
        // taken wins; the other entry finds the piece started and is let go)
        for (const piece of message.pieces) {
          queues[message.kind].push(piece);
          kinds.set(piece, message.kind);
        }
      }
      pump();
    } catch (error) {
      // (a fetcher that fails is a fetcher that is not there: the disk worker fetches for itself)
      postMessage({ method: "error", text: error.message });
    }
  };
};

/// One synchronous request for a piece's file. The bytes (whole, or null), what went wrong if they are not, and for
/// the retry rules `info`: .wait (ms the server asked for, or -1) and .fatal (not worth another try soon).
const get = (url, exact) => {
  const info = { wait: -1, fatal: false };
  try {
    // Synchronous: the task that asked is waiting, and so is everything behind it.
    const request = new XMLHttpRequest();
    request.open("GET", url, false);
    request.responseType = "arraybuffer";
    if (url.startsWith("http") && !url.startsWith(self.location.origin + "/")) {
      request.timeout = 30000;  // (not the site itself: a synchronous request to another server must not hang for ever)
    }
    request.send();
    // (The site's own file may be shorter than a piece in a disk that was cut some other way: padded below. An object
    // of the storage is named by what is in it, and is exactly the piece.)
    if (request.status == 200 && (exact ? request.response.byteLength == exact : request.response.byteLength <= piece_size)) {
      return [new Uint8Array(request.response), "", info];
    }
    if (request.status == 429 || request.status == 503) {
      try {
        info.wait = retry_after(request.getResponseHeader("Retry-After"));
      } catch (error) {
        // (no header: a cross-origin answer that does not expose it)
      }
    }
    // Other answers of the 4xx kind (not "too many", "timed out", "too early") will say the same again.
    // (A 200 of the wrong size is the same: the file on the server is not the piece.)
    info.fatal = request.status == 200 || (request.status >= 400 && request.status < 500 && ![408, 425, 429].includes(request.status));
    return [null, request.status == 200 ? request.response.byteLength + " bytes" : "status " + request.status, info];
  } catch (error) {
    return [null, error.message || error.name, info];  // (the network: transient)
  }
};

/// A piece, as piece_size bytes: from the object storage the manifest names (`remote`, content addressed:
/// REMOTE + sha256 + ".bin"), and when that fails (the network, a non-200 answer, a piece of the wrong size, a
/// browser that is not let in by its CORS rules) from the site itself, which keeps the pieces by number. A storage
/// that failed in a row is left alone for a while so that a broken one costs nothing more than a request or two (and
/// for as long as it asked, if it said "try again in n seconds"). The site's own copy is tried again with growing
/// waits, as above, until RETRY_TOTAL has passed.
const fetch_piece = (piece) => {
  const expected = Math.min(piece_size, size - piece * piece_size);  // the last piece of a disk may be short
  const started = Date.now();
  let bytes = null;
  let problem = "";
  let info;
  if (remote && hashes[piece] && Date.now() >= remote_off_until) {
    [bytes, problem, info] = get(remote + hashes[piece] + ".bin", expected);
    if (bytes) {
      remote_failures = 0;
    } else {
      if (info.wait >= 0) {
        remote_off_until = Math.max(remote_off_until, Date.now() + Math.max(info.wait, 1000));  // (the site serves meanwhile)
      }
      if (++remote_failures >= 3) {
        remote_off_until = Math.max(remote_off_until, Date.now() + 300000);
        remote_failures = 0;
        log("the object storage failed three times in a row (" + problem + "): the site itself serves the pieces for five minutes");
      }
      if (pool) {
        Atomics.store(pool.flags, FL_OFF, Math.min(2147483647, remote_off_until - started_at));  // (the fetcher leaves it alone too)
      }
    }
  }
  const url = base + String(piece).padStart(6, "0");
  let delay = RETRY_FIRST;
  let fatal = 0;
  for (let attempt = 0; !bytes; attempt++) {
    [bytes, problem, info] = get(url, 0);
    if (bytes) {
      break;
    }
    if (info.fatal && ++fatal >= FATAL_TRIES) {
      throw new Error("cannot fetch " + url + ": " + problem);
    }
    // What the server asked for (plus a little jitter, so that many machines released at once do not come back at once),
    // or the doubling wait with full jitter in its upper half.
    const wait = info.wait >= 0 ? Math.min(RETRY_MAX, Math.max(250, info.wait) * (1 + 0.2 * Math.random()))
                                : delay * (0.5 + 0.5 * Math.random());
    delay = Math.min(RETRY_MAX, delay * 2);
    if (Date.now() + wait - started > RETRY_TOTAL) {
      throw new Error("cannot fetch " + url + ": " + problem + " (tried for " + Math.round((Date.now() - started) / 1000) + " s)");
    }
    if (attempt == 0) {
      log("piece " + piece + ": " + problem + ", trying again (waits of " + (Math.round(wait / 100) / 10) + " s and more)");
    }
    sleep(wait);
  }
  if (bytes.byteLength == piece_size) {
    return bytes;
  }
  const padded = new Uint8Array(piece_size);
  padded.set(bytes);
  return padded;
};

/// What a file that the browser has taken away answers with (measured in Chrome 154, a private window's file system, after
/// the origin's data was removed): read() and write() return -4 (base::File's FILE_ERROR_NOT_FOUND) as an unsigned
/// 32-bit number, 4294967292; getSize() returns 2^64 - 4; a directory handle from before and the root's
/// getDirectoryHandle(name) say NotFoundError. (A disk-backed profile keeps serving an open handle after its data is
/// removed, the file being unlinked: nothing to see from here, the page notices that by the directory.) Out of room
/// is -8 (FILE_ERROR_NO_SPACE), as a short write, in a private window, and a QuotaExceededError elsewhere.
const FILE_NOT_FOUND = -4, FILE_NO_SPACE = -8;
/// The error code a count stands for (the count is an unsigned 32-bit number), or 0 if it is a count.
const file_error = (count) => (count > 0x7fffffff ? count | 0 : 0);
const is_gone = (error) => !!error && (error.name == "NotFoundError" || error.name == "InvalidStateError");
/// The Error for a count that is an error code.
const count_error = (count) => {
  const code = file_error(count);
  return Object.assign(new Error(code == FILE_NOT_FOUND ? "the file is gone (" + code + ")"
                                 : code == FILE_NO_SPACE ? "no room (" + code + ")" : "file error " + code),
                       { name: code == FILE_NOT_FOUND ? "NotFoundError" : code == FILE_NO_SPACE ? "QuotaExceededError" : "FileError" });
};
/// Is the open file still there? (A cheap look.)
const file_there = (handle) => {
  try {
    return handle.getSize() < 2 ** 53;
  } catch (error) {
    return !is_gone(error);
  }
};

/// The browser has taken the files away, or they fail in a way that cannot be mended (see "When the browser takes the
/// files away"): go on from memory, once, and tell the page. `error` is what was seen first, `where` which call.
const storage_lost = (error, where) => {
  if (lost) {
    return;
  }
  lost = true;
  const at = Date.now();
  for (const handle of [data, map]) {
    try {
      handle.close();
    } catch (ignored) {
    }
  }
  data = null;
  map = null;
  unsaved = 0;
  if (pool) {
    Atomics.store(pool.flags, FL_STOP, 1);
  }
  stop_pool();  // (pieces fetched ahead would only fill memory)
  let again = 0, copies = 0, zeros = 0, dirty_lost = 0;
  for (let piece = 0; piece < place.length; piece++) {
    if (!place[piece]) {
      continue;
    }
    place[piece] = 0;
    const copy = mirror.get(piece);
    if (copy) {
      in_memory.set(piece, copy);  // (what was in the file, whole)
      copies++;
    } else if (!keep_disk && present[piece]) {
      again++;  // (the server has it)
      if (dirty.has(piece)) {
        dirty_lost++;
      }
    } else {
      zeros++;  // (nothing to make it from: what was never read in this visit, of a disk that is somebody's own)
    }
  }
  mirror.clear();
  const text = (error && error.name || "error") + (error && error.message ? ": " + error.message : "") + (where ? " in " + where : "");
  log("the browser removed this site's storage (" + text + "): this disk goes on from memory only. "
      + copies + " parts kept in memory" + (keep_disk ? "" : ", " + again + " fetched again when wanted")
      + (zeros || dirty_lost ? ", " + (zeros + dirty_lost) + " could not be kept" : "")
      + "; nothing is stored again in this visit");
  if (channel) {
    try {
      channel.postMessage({ type: "lost", disk: disk_name, keep: keep_disk, at: at, saved_at: saved_at, kept: copies,
                            fetched_again: keep_disk ? 0 : again, gone: zeros + dirty_lost, reason: text });
    } catch (ignored) {
    }
  }
};

/// A copy of the piece that is in the file, in memory, so that it is not lost with the file: of every piece that is
/// touched on a disk that is somebody's own, of the pieces that are written to on a disk of the image. Not beyond
/// the bytes there are for them. (A piece that was fetched only is not copied: the server has it.)
const mirror_piece = (piece) => {
  if (!data || mirror.has(piece) || !place[piece] || (mirror.size + 1) * piece_size > (keep_disk ? MIRROR_KEEP : MIRROR_BASE)) {
    return;
  }
  const copy = new Uint8Array(piece_size);
  try {
    const done = data.read(copy, { at: (place[piece] - 1) * piece_size });
    if (done == piece_size) {
      mirror.set(piece, copy);
    } else if (file_error(done) == FILE_NOT_FOUND) {
      storage_lost(count_error(done), "reading a part");
    }
  } catch (error) {
    if (is_gone(error)) {
      storage_lost(error, "reading a part");
    }
  }
};

/// Pieces that are in memory only and that can be fetched again (a disk of the image, not written to): only so many;
/// the ones used least lately go. (A Map is in the order it was added in: a piece that is used again is added anew.)
const trim_memory = (newest) => {
  if (keep_disk || in_memory.size <= MEMORY_CLEAN) {
    return;
  }
  for (const [piece] of in_memory) {
    if (in_memory.size <= MEMORY_CLEAN) {
      break;
    }
    if (piece != newest && !dirty.has(piece) && present[piece]) {
      in_memory.delete(piece);
    }
  }
};

/// Put the bytes of a piece where pieces are kept: a place in `data`, or memory if that cannot be.
const keep_piece = (piece, bytes) => {
  if (data) {
    try {
      const done = data.write(bytes, { at: places * piece_size });
      if (done == piece_size) {
        place[piece] = ++places;
        unsaved++;
        if (keep_disk && (mirror.size + 1) * piece_size <= MIRROR_KEEP) {
          mirror.set(piece, bytes);  // (nobody else changes these bytes: they follow the writes, see transfer)
        }
        return;
      }
      if (file_error(done)) {
        throw count_error(done);
      }
      throw new Error("short write");
    } catch (error) {
      if (is_gone(error)) {
        storage_lost(error, "writing a part");
      } else if (!complained) {
        // Most likely the origin's quota (QuotaExceededError). The system goes on with this piece in memory.
        complained = true;
        log("cannot store more of the disk (" + error.name + ": " + error.message
            + "); from here on, new parts are kept in memory only");
        if (pool) {
          Atomics.store(pool.flags, FL_STOP, 1);  // (nothing more is fetched ahead: it would only fill memory)
        }
        if (channel) {
          // (The store of translations (jitcache.js) takes its file out of the way: the disks come first.)
          try {
            channel.postMessage({ type: "nospace", disk: disk_name, reason: error.name });
          } catch (ignored) {
          }
        }
      }
    }
  }
  in_memory.set(piece, bytes);
  trim_memory(piece);
};

/// The piece in slot `slot` (it is READY) into the files, and the slot free again.
const take = (slot, piece) => {
  // (`pool` can be gone when keep_piece returns: if the files were taken away it ended the pool, see storage_lost.)
  const shared = pool, worker = pool_worker;
  const bytes = new Uint8Array(piece_size);
  bytes.set(new Uint8Array(shared.slots, slot * piece_size, piece_size));
  keep_piece(piece, bytes);
  Atomics.store(shared.state, piece, ST_HAVE);
  Atomics.store(shared.slot_piece, slot, 0);  // (free only now: at most SLOTS pieces wait for the files)
  if (worker) {
    worker.postMessage({ op: "kick" });
  }
};

/// A piece that the fetcher has brought or has in flight, if it has: waits for it (a piece is asked for once, and a
/// request in flight is as good as one of this worker's own) and takes it. Otherwise (or if the wait was too long,
/// or the fetcher failed) the piece is this worker's own to fetch: false.
const from_pool = (piece) => {
  const deadline = Date.now() + wait_flying;
  for (;;) {
    const state = Atomics.load(pool.state, piece);
    if (state == ST_READY) {
      const slot = Atomics.load(pool.slot_of, piece);
      if (Atomics.load(pool.slot_piece, slot) == piece + 1) {
        take(slot, piece);
        return true;
      }
      Atomics.store(pool.state, piece, ST_MINE);  // (cannot happen)
      return false;
    }
    if (state == ST_FLYING) {
      const left = deadline - Date.now();
      if (left <= 0) {
        Atomics.compareExchange(pool.state, piece, ST_FLYING, ST_MINE);
        return false;
      }
      Atomics.wait(pool.state, piece, ST_FLYING, left);
      continue;
    }
    if (state == ST_QUEUED && Atomics.compareExchange(pool.state, piece, ST_QUEUED, ST_MINE) != ST_QUEUED) {
      continue;  // (it was started this very moment)
    }
    Atomics.store(pool.state, piece, ST_MINE);
    return false;
  }
};

/// Fetching ahead is an extra: whatever goes wrong with it, it stops, and the disk goes on without it.
const pool_broke = (where, error) => {
  log("fetching ahead stopped (" + where + ": " + (error && error.message || error) + "): pieces are fetched one at a time");
  const worker = pool_worker;
  pool = null;
  pool_worker = null;
  try {
    worker.terminate();
  } catch (ignored) {
  }
};

/// Have the piece here: fetch it, or make it out of zeros, and give it a place.
const bring = (piece) => {
  if (place[piece] || in_memory.has(piece)) {
    how = "kept";
    return;
  }
  if (pool && present[piece]) {
    // (Counted against the fetcher's limit from here: while the machine waits, the fetcher has one request fewer.)
    const mine = pool, worker = pool_worker;
    if (serving) {
      Atomics.add(mine.flags, FL_DEMAND, 1);
    }
    try {
      let got = false;
      try {
        got = from_pool(piece);
      } catch (error) {
        pool_broke("waiting for a piece", error);
      }
      if (got) {
        how = "pool";
        return;
      }
      how = "demand";
      keep_piece(piece, fetch_piece(piece));
      Atomics.store(mine.state, piece, ST_HAVE);
      return;
    } catch (error) {
      Atomics.store(mine.state, piece, ST_FAILED);
      throw error;
    } finally {
      if (serving) {
        Atomics.sub(mine.flags, FL_DEMAND, 1);
        worker.postMessage({ op: "kick" });
      }
    }
  }
  how = "demand";
  keep_piece(piece, present[piece] ? fetch_piece(piece) : new Uint8Array(piece_size));
};

/// Put these pieces (the ones nobody has asked for yet) in the fetcher's queue, as `kind`. How many were new.
const enqueue = (list, kind) => {
  const fresh = [];
  for (const piece of list) {
    if (Atomics.compareExchange(pool.state, piece, ST_IDLE, ST_QUEUED) == ST_IDLE) {
      fresh.push(piece);
    }
  }
  if (fresh.length) {
    pool_worker.postMessage({ op: "enqueue", pieces: fresh, kind: kind });
  }
  return fresh.length;
};

/// Pieces asked for ahead of a run: the new ones are queued, the ones that wait at the end of the boot list move up.
const ahead_of = (list) => {
  const queued = list.filter((piece) => Atomics.load(pool.state, piece) == ST_QUEUED && !pool.promoted.has(piece));
  enqueue(list, KIND_AHEAD);
  if (queued.length) {
    for (const piece of queued) {
      pool.promoted.add(piece);
    }
    pool_worker.postMessage({ op: "promote", pieces: queued, kind: KIND_AHEAD });
  }
};

/// The pieces of a run of consecutive ones are read one after the other: when a piece comes right after another one,
/// the next AHEAD_FIRST are asked for, then 8, then 16 (AHEAD_MAX) as the run goes on. (A few runs at once: a
/// start-up reads two or three files side by side.)
const streams = [];
const touched = (piece) => {
  if (!pool || !serving || complained) {
    return;
  }
  const stream = streams.find((s) => piece > s.last && piece - s.last <= 2);
  if (!stream) {
    if (streams.length >= 4) {
      streams.shift();
    }
    streams.push({ last: piece, ahead: 0 });
    return;
  }
  stream.last = piece;
  stream.ahead = stream.ahead ? Math.min(AHEAD_MAX, stream.ahead * 2) : AHEAD_FIRST;
  const list = [];
  for (let next = piece + 1; next <= piece + stream.ahead && next < present.length; next++) {
    list.push(next);
  }
  ahead_of(list);
};

/// Put one piece the fetcher has brought into the files (if one is there). True if it did.
const drain = () => {
  for (let slot = 0; slot < SLOTS; slot++) {
    const piece = Atomics.load(pool.slot_piece, slot) - 1;
    if (piece >= 0 && Atomics.load(pool.state, piece) == ST_READY) {
      take(slot, piece);
      return true;
    }
  }
  return false;
};

/// Start the fetcher (its script is this one, named "disk-fetcher"). It takes a moment to come up, which is spent on the
/// manifest and the files; `start_pool` below waits for it.
const make_fetcher = () => {
  if (limit > 0 && typeof Worker == "function") {
    try {
      pool_worker = new Worker(self.location.href, { name: "disk-fetcher" });
      // An error of the fetcher's is its own: left alone, it would be reported as this worker's, and the page takes a
      // disk worker that has an error for no disk.
      pool_worker.onerror = (event) => {
        if (event && event.preventDefault) {
          event.preventDefault();
        }
      };
    } catch (error) {
      pool_worker = null;
    }
  }
};

/// Give the fetcher the tables, once there are files to keep what it fetches in and a place to fetch from. False (and
/// no fetcher) if it does not come up, or there is nothing for it to do.
const start_pool = async (boot) => {
  try {
    return await start_pool_now(boot);
  } catch (error) {
    pool_broke("starting", error);
    return false;
  }
};
const start_pool_now = async (boot) => {
  const worker = pool_worker;
  if (!worker || !data || !remote) {
    stop_pool();
    return false;
  }
  const pieces = present.length;
  const table = (words) => new Int32Array(new SharedArrayBuffer(words * 4));
  const shared = {
    op: "init", control: control, state: table(pieces), slot_of: table(pieces), slot_piece: table(SLOTS), flags: table(4),
    slots: new SharedArrayBuffer(SLOTS * piece_size), limit: limit, piece_size: piece_size, size: size,
    remote: remote, hashes: hashes, started_at: started_at,
  };
  for (let piece = 0; piece < pieces; piece++) {
    // (What is here, what is all zeros and what has no object to fetch is not for the fetcher.)
    shared.state[piece] = place[piece] || !present[piece] || !hashes[piece] ? ST_HAVE : ST_IDLE;
  }
  const came_up = await new Promise((resolve) => {
    const timer = setTimeout(() => resolve(false), 5000);
    worker.onmessage = (event) => {
      if (event.data.method == "up") {
        clearTimeout(timer);
        resolve(true);
      }
    };
    worker.onerror = (event) => {
      if (event && event.preventDefault) {
        event.preventDefault();
      }
      clearTimeout(timer);
      resolve(false);
    };
    worker.postMessage(shared);
  });
  if (!came_up) {
    log("no fetcher (it did not come up): pieces are fetched one at a time");
    stop_pool();
    return false;
  }
  worker.onmessage = null;
  worker.onerror = (event) => {
    if (event && event.preventDefault) {
      event.preventDefault();
    }
  };
  pool = shared;
  pool.promoted = new Set();
  if (boot) {
    const list = boot.filter((piece) => Number.isInteger(piece) && piece >= 0 && piece < pieces);
    const new_ones = enqueue(list, KIND_BOOT);
    log("fetching ahead, " + limit + " at a time: " + new_ones + " of the " + list.length + " pieces of a boot are not here yet");
  }
  return true;
};

const stop_pool = () => {
  if (pool_worker) {
    pool_worker.terminate();
  }
  pool_worker = null;
  pool = null;
};

/// Bring `map` up to date with `data`, and both to where they survive. If the files fail, they are lost (storage_lost).
const save = () => {
  if (!data || !unsaved) {
    return;
  }
  try {
    data.flush();
    const done = map.write(place, { at: HEADING });
    if (file_error(done)) {
      throw count_error(done);
    }
    map.flush();
    unsaved = 0;
    saved_at = Date.now();
  } catch (error) {
    storage_lost(error, "saving");
  }
};

let last_piece = -1;

/// Read or write `length` bytes at `offset` of the disk, from or to the machine's memory at `address`.
const transfer = (write, offset, address, length) => {
  if (offset + length > size) {
    throw new Error("beyond the end of the disk");
  }
  while (length > 0) {
    const piece = Math.floor(offset / piece_size);
    const within = offset - piece * piece_size;
    const count = Math.min(length, piece_size - within);
    // (A new view every time: the memory's buffer is another object after the memory has grown.)
    const view = new Uint8Array(memory.buffer, address, count);

    if (!write && !place[piece] && !in_memory.has(piece) && !present[piece]) {
      view.fill(0);  // nothing was ever there
    } else {
      const began = trace_on ? Date.now() : 0;
      bring(piece);
      if (trace_on && !traced[piece]) {
        traced[piece] = 1;
        log("trace " + (began - started_at) + " " + disk_name + " " + piece + " " + (how == "kept" ? -1 : Date.now() - began) + " " + how);
      }
      if (pool && (piece != last_piece)) {
        last_piece = piece;
        try {
          touched(piece);
        } catch (error) {
          pool_broke("read-ahead", error);
        }
      }
      // (Twice at most: the files can be taken away in the middle of this, and then the piece is made again, in memory.)
      for (let attempt = 0; attempt < 2; attempt++) {
        const bytes = in_memory.get(piece);
        if (bytes) {
          if (write) {
            bytes.set(view, within);
            dirty.add(piece);
          } else {
            view.set(bytes.subarray(within, within + count));
          }
          if (!keep_disk) {
            in_memory.delete(piece);  // (the most lately used goes last, see trim_memory)
            in_memory.set(piece, bytes);
          }
          break;
        }
        const at = (place[piece] - 1) * piece_size + within;
        let done;
        try {
          if (write) {
            dirty.add(piece);
            mirror_piece(piece);  // (before the write: the copy of what was there)
          } else if (keep_disk) {
            mirror_piece(piece);
          }
          if (lost) {
            bring(piece);
            continue;
          }
          if (direct) {
            try {
              done = write ? data.write(view, { at: at }) : data.read(view, { at: at });
            } catch (error) {
              if (!(error instanceof TypeError)) {
                throw error;
              }
              direct = false;  // this browser does not take shared memory here: by way of a copy, then
            }
          }
          if (!direct) {
            const copy = new Uint8Array(count);
            if (write) {
              copy.set(view);
              done = data.write(copy, { at: at });
            } else {
              done = data.read(copy, { at: at });
              if (!file_error(done)) {
                view.set(copy);
              }
            }
          }
          if (file_error(done)) {
            throw count_error(done);
          }
        } catch (error) {
          if (!is_gone(error) || attempt > 0) {
            throw error;
          }
          storage_lost(error, write ? "a write" : "a read");
          bring(piece);  // (from the image again, or from the copy in memory)
          continue;
        }
        if (done != count) {
          throw new Error((write ? "wrote " : "read ") + done + " of " + count + " bytes");
        }
        const copy = write ? mirror.get(piece) : null;
        if (copy) {
          copy.set(view, within);  // (the copy of the piece follows what is written)
        }
        break;
      }
    }
    offset += count;
    address += count;
    length -= count;
  }
};

/// Open the two files, or leave `data` null. `heading` names the image they must belong to. Returns whether
/// what was there from before was kept.
const open_files = async (name, heading, reset) => {
  let handles = [];
  try {
    const root = await navigator.storage.getDirectory();
    const directory = await root.getDirectoryHandle("disk-" + name, { create: true });
    for (const file of ["data", "map"]) {
      const handle = await directory.getFileHandle(file, { create: true });
      // A page that was just reloaded may find the files still locked by the worker it had before: the browser
      // lets go of them a little after that worker is gone.
      for (let attempt = 0; ; attempt++) {
        try {
          handles.push(await handle.createSyncAccessHandle());
          break;
        } catch (error) {
          if (error.name != "NoModificationAllowedError" || attempt == 40) {
            throw error;
          }
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
      }
    }
  } catch (error) {
    for (const handle of handles) {
      handle.close();
    }
    log(error.name == "NoModificationAllowedError"
        ? "the disk is in use by another tab: this one keeps its changes in memory only"
        : "no private file system for the disk (" + error.name + ": " + error.message
          + "): changes are kept in memory only");
    return false;
  }
  [data, map] = handles;
  saved_at = Date.now();

  // What is there from before counts if it is of this image and whole.
  const found = new Uint8Array(HEADING);
  if (!reset && map.getSize() == HEADING + place.byteLength && map.read(found, { at: 0 }) == HEADING
      && found.every((byte, i) => byte == heading[i]) && map.read(place, { at: HEADING }) == place.byteLength) {
    const stored = Math.floor(data.getSize() / piece_size);
    let kept = 0;
    for (let piece = 0; piece < place.length; piece++) {
      if (place[piece] > stored) {
        place[piece] = 0;  // (cannot happen while `map` is written after `data`; if it does, fetch again)
      } else if (place[piece]) {
        places = Math.max(places, place[piece]);
        kept++;
      }
    }
    data.truncate(places * piece_size);  // pieces that arrived but never made it into `map`
    log("kept from before: " + Math.round(kept * piece_size / 1048576) + " MiB of "
        + Math.round(size / 1048576) + " MiB");
    return true;
  } else {
    const had = map.getSize() > 0;
    place.fill(0);
    data.truncate(0);
    map.truncate(0);
    map.write(heading, { at: 0 });
    map.write(place, { at: HEADING });
    map.flush();
    log(reset ? "started afresh, as asked" : had ? "the image on the server is another one: started afresh"
        : "new: parts are fetched as they are needed, and kept");
    return false;
  }
};

/// Serve requests, for ever.
const serve = () => {
  serving = true;
  let idle_since = Date.now();
  for (;;) {
    // Only a request that is there: a wait can end on the late notify of the request before, which this
    // worker saw and served without waiting (tools/patch-runtime.py 14). Serving it again would write the
    // old sectors from a buffer that holds something else now, or read into one.
    if (Atomics.load(control, REQUEST) != 1) {
      let drained = false;
      if (pool) {
        try {
          drained = drain();
        } catch (error) {
          pool_broke("writing a piece", error);
        }
      }
      if (drained) {
        // (A piece the fetcher brought into the files; then another look at the request words before the next.)
        idle_since = Date.now();
        if (unsaved >= 64) {
          save();
        }
        continue;
      }
      // Nothing to do: wait for a request. With something unsaved, for a second without one; with a fetcher, never
      // for long (it wakes this worker when a piece is ready, but a wake can come in the moment before the wait).
      // (With files: not for ever either, every PROBE_EVERY ms a look whether the browser still has them.)
      const patience = unsaved ? Math.max(1, 1000 - (Date.now() - idle_since)) : data ? PROBE_EVERY : Infinity;
      if (Atomics.wait(control, REQUEST, 0, pool ? Math.min(patience, 250) : patience) == "timed-out") {
        if (unsaved && Date.now() - idle_since >= 1000) {
          save();
        }
        if (data && Date.now() - probed_at >= PROBE_EVERY) {
          probed_at = Date.now();
          if (!file_there(data) || !file_there(map)) {
            storage_lost(count_error(FILE_NOT_FOUND >>> 0), "a look at the files");
          }
        }
      }
      continue;
    }
    idle_since = Date.now();
    let result = 0;
    try {
      const op = control[OP];
      if (op == OP_FLUSH) {
        if (unsaved) {
          save();
        } else if (data) {
          try {
            data.flush();  // what was written into pieces that were here already
            saved_at = Date.now();
          } catch (error) {
            if (!is_gone(error)) {
              throw error;
            }
            storage_lost(error, "a flush");
          }
        }
      } else {
        const sector = (control[SECTOR_LOW] >>> 0) + control[SECTOR_HIGH] * 4294967296;
        const address = (control[ADDRESS] >>> 0) + (control[ADDRESS_HIGH] >>> 0) * 4294967296;
        transfer(op == OP_WRITE, sector * SECTOR, address, control[COUNT] * SECTOR);
      }
    } catch (error) {
      // (One line for the first, and a count now and then: a disk that fails fails at every request.)
      log_limited(String(error.message).replace(/\d+/g, "#"), "error: " + error.message);
      result = -EIO;
    }
    control[RESULT] = result;
    Atomics.store(control, REQUEST, 0);
    Atomics.store(control, DONE, 1);
    Atomics.notify(control, DONE);
    if (unsaved >= 64) {
      save();
    }
  }
};

const disk_main = async (event) => {
  const message = event.data;
  try {
    memory = message.memory;
    control = message.control;
    disk_name = message.name;
    keep_disk = !!message.keep;
    base = message.url;
    if (!message.keep) {
      make_fetcher();  // (the home disk's is made when it is known that it has pieces to copy)
    }
    const manifest = await (await fetch(base + "manifest.json", { cache: "no-store" })).json();
    size = manifest.size;
    piece_size = manifest.piece;
    const pieces = Math.ceil(size / piece_size);
    // Pieces from object storage need both an address and the hashes to name them by; a manifest without either is
    // served by the site itself, as it always was.
    remote = "";
    if (manifest.base && Array.isArray(manifest.hashes) && manifest.hashes.length == pieces) {
      remote = new URL(manifest.base.endsWith("/") ? manifest.base : manifest.base + "/", self.location.href).href;
      hashes = manifest.hashes;
    }
    present = new Uint8Array(pieces);
    for (let piece = 0; piece < pieces; piece++) {
      present[piece] = (parseInt(manifest.present[piece >> 2], 16) >> (3 - (piece & 3))) & 1;
    }
    place = new Uint32Array(pieces);
    traced = new Uint8Array(pieces);

    // What the files here must say they are of. A kept disk is of no image: "kept", once it is whole.
    const heading_of = (id) => {
      const heading = new Uint8Array(HEADING).fill(32);
      heading.set(new TextEncoder().encode("rvdisk 1 " + id + " " + piece_size + " " + pieces));
      return heading;
    };
    if (!message.keep) {
      await open_files(message.name, heading_of(manifest.id), message.reset);
      // The pieces of a cold boot (`boot`, in the order they are read) are fetched ahead, from the object storage.
      await start_pool(Array.isArray(manifest.boot) ? manifest.boot : null);
    } else if (!await open_files(message.name, heading_of("kept"), message.reset)) {
      // New here (or not kept at all, in memory): everything the image has, now. Until that is done the
      // files say so, and a page that finds them like that starts over.
      if (map) {
        try {
          map.write(heading_of("copying"), { at: 0 });
          map.flush();
        } catch (error) {
          storage_lost(error, "starting the copy");
        }
      }
      // (All of them at once, if there is a fetcher: the machine does not start until they are here.)
      make_fetcher();
      if (await start_pool(null)) {
        const all = [];
        for (let piece = 0; piece < pieces; piece++) {
          if (present[piece]) {
            all.push(piece);
          }
        }
        enqueue(all, KIND_COPY);
      }
      for (let piece = 0; piece < pieces; piece++) {
        if (present[piece]) {
          bring(piece);
        }
      }
      stop_pool();
      if (map) {
        save();
        try {
          map.write(heading_of("kept"), { at: 0 });
          map.flush();
        } catch (error) {
          storage_lost(error, "finishing the copy");
        }
      }
    } else if (data && message.keep) {
      // A disk that is somebody's own, from before: a copy of it in memory (if it is small enough) so that it can go on
      // if the browser takes its files away. Nothing else has what is on it.
      for (let piece = 0; piece < place.length && !lost; piece++) {
        if (place[piece]) {
          mirror_piece(piece);
        }
      }
    }
    if (message.keep) {
      present.fill(0);  // what is not here is zeros
    }

    const sectors = size / SECTOR;
    control[SECTORS_LOW] = sectors % 4294967296 | 0;
    control[SECTORS_HIGH] = Math.floor(sectors / 4294967296);
    postMessage({ method: "ready", kept: data != null });
  } catch (error) {
    postMessage({ method: "failed", text: error.name + ": " + error.message });
    return;
  }
  serve();
};

if (is_fetcher) {
  fetcher_main();
} else {
  onmessage = disk_main;
}
