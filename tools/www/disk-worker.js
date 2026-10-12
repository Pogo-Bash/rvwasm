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
// The files are opened in the ordinary way, which locks them: a second tab
// with the same disk cannot have them, and neither can this worker if OPFS
// is missing or refuses. Then the pieces are kept in memory instead and are
// gone when the page is: the system works, and forgets.
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
let base = "";          // where the pieces are fetched from
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

const log = (text) => postMessage({ method: "log", text: text });

/// A piece from the server, as piece_size bytes.
const fetch_piece = (piece) => {
  const url = base + String(piece).padStart(6, "0");
  let problem = "";
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      // Synchronous: the task that asked is waiting, and so is everything behind it.
      const request = new XMLHttpRequest();
      request.open("GET", url, false);
      request.responseType = "arraybuffer";
      request.send();
      if (request.status == 200 && request.response.byteLength <= piece_size) {
        if (request.response.byteLength == piece_size) {
          return new Uint8Array(request.response);
        }
        const bytes = new Uint8Array(piece_size);  // the last piece of a disk may be short
        bytes.set(new Uint8Array(request.response));
        return bytes;
      }
      problem = "status " + request.status;
    } catch (error) {
      problem = error.message;
    }
  }
  throw new Error("cannot fetch " + url + ": " + problem);
};

/// Have the piece here: fetch it, or make it out of zeros, and give it a place.
const bring = (piece) => {
  if (place[piece] || in_memory.has(piece)) {
    return;
  }
  const bytes = present[piece] ? fetch_piece(piece) : new Uint8Array(piece_size);
  if (data) {
    try {
      if (data.write(bytes, { at: places * piece_size }) == piece_size) {
        place[piece] = ++places;
        unsaved++;
        return;
      }
      throw new Error("short write");
    } catch (error) {
      // Most likely the origin's quota (QuotaExceededError). The system goes on with this piece in memory.
      if (!complained) {
        complained = true;
        log("cannot store more of the disk (" + error.name + ": " + error.message
            + "); from here on, new parts are kept in memory only");
      }
    }
  }
  in_memory.set(piece, bytes);
};

/// Bring `map` up to date with `data`, and both to where they survive.
const save = () => {
  if (!data || !unsaved) {
    return;
  }
  data.flush();
  map.write(place, { at: HEADING });
  map.flush();
  unsaved = 0;
};

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
      bring(piece);
      const bytes = in_memory.get(piece);
      if (bytes) {
        if (write) {
          bytes.set(view, within);
        } else {
          view.set(bytes.subarray(within, within + count));
        }
      } else {
        const at = (place[piece] - 1) * piece_size + within;
        let done;
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
            view.set(copy);
          }
        }
        if (done != count) {
          throw new Error((write ? "wrote " : "read ") + done + " of " + count + " bytes");
        }
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
  for (;;) {
    if (Atomics.wait(control, REQUEST, 0, unsaved ? 1000 : Infinity) == "timed-out") {
      save();
      continue;
    }
    // Only a request that is there: a wait can end on the late notify of the request before, which this
    // worker saw and served without waiting (tools/patch-runtime.py 14). Serving it again would write the
    // old sectors from a buffer that holds something else now, or read into one.
    if (Atomics.load(control, REQUEST) != 1) {
      continue;
    }
    let result = 0;
    try {
      const op = control[OP];
      if (op == OP_FLUSH) {
        if (unsaved) {
          save();
        } else if (data) {
          data.flush();  // what was written into pieces that were here already
        }
      } else {
        const sector = (control[SECTOR_LOW] >>> 0) + control[SECTOR_HIGH] * 4294967296;
        const address = (control[ADDRESS] >>> 0) + (control[ADDRESS_HIGH] >>> 0) * 4294967296;
        transfer(op == OP_WRITE, sector * SECTOR, address, control[COUNT] * SECTOR);
      }
    } catch (error) {
      log("error: " + error.message);
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

onmessage = async (event) => {
  const message = event.data;
  try {
    memory = message.memory;
    control = message.control;
    base = message.url;
    const manifest = await (await fetch(base + "manifest.json", { cache: "no-store" })).json();
    size = manifest.size;
    piece_size = manifest.piece;
    const pieces = Math.ceil(size / piece_size);
    present = new Uint8Array(pieces);
    for (let piece = 0; piece < pieces; piece++) {
      present[piece] = (parseInt(manifest.present[piece >> 2], 16) >> (3 - (piece & 3))) & 1;
    }
    place = new Uint32Array(pieces);

    // What the files here must say they are of. A kept disk is of no image: "kept", once it is whole.
    const heading_of = (id) => {
      const heading = new Uint8Array(HEADING).fill(32);
      heading.set(new TextEncoder().encode("rvdisk 1 " + id + " " + piece_size + " " + pieces));
      return heading;
    };
    if (!message.keep) {
      await open_files(message.name, heading_of(manifest.id), message.reset);
    } else if (!await open_files(message.name, heading_of("kept"), message.reset)) {
      // New here (or not kept at all, in memory): everything the image has, now. Until that is done the
      // files say so, and a page that finds them like that starts over.
      if (map) {
        map.write(heading_of("copying"), { at: 0 });
        map.flush();
      }
      for (let piece = 0; piece < pieces; piece++) {
        if (present[piece]) {
          bring(piece);
        }
      }
      if (map) {
        save();
        map.write(heading_of("kept"), { at: 0 });
        map.flush();
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
