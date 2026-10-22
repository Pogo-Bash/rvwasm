# Storage that is small or goes away

What the page keeps in the browser's origin private file system (OPFS) is the disks' pieces (`tools/www/disk-worker.js`: the
Debian system, the native libraries, and the person's own `home` disk) and the store of translated code
(`tools/www/jitcache.js`, 100 to 200 MiB). This is what happens to them when the browser has little room, or takes the
storage away in the middle of a visit, and what the page does about it.

## What was seen (2026-10-13, owner, Chrome Guest window, /desktop)

Mid-session the log showed `[JIT cache]: not saved (A requested file or directory could not be found at the time an
operation was processed.)`, then hundreds of `[Disk release]: error: read 4294967292 of 4096 bytes` and `[Disk home-home]:
error: wrote 4294967292 of 4096 bytes`. 4294967292 is -4 as an unsigned 32-bit number; Chrome's `base::File::Error` has
`FILE_ERROR_NOT_FOUND = -4`. The cache's `NotFoundError` is the root directory handle the page held, gone.

## What Chrome does (measured with `tools/storage-probe.js`, Chrome for Testing 154, macOS, 36 GB)

| | default profile | private context (`Target.createBrowserContext`), as a private or Guest window |
|---|---|---|
| file system | on disk | **in memory** |
| `estimate()` | `quota = usage + 10 GiB` | the same: `quota = usage + 10 GiB`, with a real cap of **about 2000 MiB** (short write of 4294967288 = -8, no room); a cap set with `Storage.overrideQuotaForOrigin` is not shown either |
| past the cap | `QuotaExceededError` thrown by `write()` | `write()` returns 4294967288 (a short write, no exception); creating a directory throws `QuotaExceededError` |
| origin's data removed (`Storage.clearDataForOrigin`) | an open handle goes on working (the file is unlinked); a new `getDirectoryHandle(name)` says `NotFoundError` | an open handle's `read()` and `write()` return **4294967292**, `getSize()` returns 2^64 - 4, `getDirectoryHandle(name)` says `NotFoundError`: exactly the owner's log |
| data removed by running out of room | not tried | **not done by Chrome by itself**: 90 s after a write failed for want of room the data was all still there |

Chrome's own sources say the same about the size: a profile that is off the record gets a pool of 15 to 20% of the computer's
memory, a third of it per site (`storage/browser/quota/quota_settings.cc`), so the 2000 MiB seen here is a 36 GB computer's;
a 16 GB computer's is about 0.8 to 1.1 GiB, which is less than one visit to the Debian desktop reads (the manifest's boot
list alone is 812 pieces of 1 MiB). Site data is best effort and can be evicted whole (`EvictBucketData` deletes all of a
bucket's data), and a private profile never gets `persist()`. Why the data went in the owner's session is not known:
the removal of an off-the-record profile's file system explains every symptom, but what removed it (eviction, clearing the
site's data, the browser's own cleanup) could not be made to happen here. So the page is built to survive it, not to prevent it.

Two more measurements shaped the cache: `createWritable({ keepExistingData: true })` **copies the file into a swap file**
(a 40 MiB file: usage 40 MiB, then 80 MiB while the writable is open, then 40 again); the cache used it for every save
(107 MiB in the owner's log, so 214 MiB at the moment of the save, every minute something was added).
And `navigator.storage.estimate()` cannot be used to learn a small quota in Chrome (table above).

## Reproduced (2026-10-13, headless Chrome for Testing 154, `tools/jitdesk.js`, private context)

The default desktop booted from the live dist, Konsole opened, then the origin's file systems were removed
(`Storage.clearDataForOrigin`). Before: 748,771 lines `[Disk release]: error: read 4294967292 of 4096 bytes` in the log,
Dolphin never got a window. After: one line per disk, the note shown, Dolphin, Kate and Konsole again opened (13 s, 8 s,
4 s), no `error:` lines. With the origin capped at 700 MiB the disks and the cache back off quietly and all four programs
open. On a normal profile the whole gate passes (Konsole, Dolphin, Kate, Konsole again).

## What the page does now

**Disks** (`disk-worker.js`, header comment "When the browser takes the files away"). A request that gets an error code
(-4), a `NotFoundError`/`InvalidStateError`, or a look at the files every 5 s while idle (`getSize()` beyond 2^53)
means the files are gone, once. Then:

- a disk of the server's image (Debian root, native libraries): every piece that was only fetched is fetched again from the
  server when it is next wanted (content addressed, so the machine goes on as it was); the pieces this visit wrote to
  were copied to memory at their first write and go on from there. What an earlier visit had written to a piece is not known
  and is the image's again, so a disk that was changed a lot over days (apt installs) can be inconsistent afterwards:
  this is the one thing the page cannot repair; the next visit starts that disk afresh anyway;
- the person's own disk (`home`): all of it was copied to memory at the start if it is small (up to 256 MiB; beyond that
  the pieces touched in the visit), so after the loss it is that copy;
- pieces held in memory that can be fetched again are limited to 160 per disk (the least lately used go);
- nothing is written to the browser's storage again in that visit;
- one log line and one message on the `BroadcastChannel` `rvwasm-storage` (`lost`), not one line per failed request
  (other repeated errors are counted: the first, then the 10th, 100th, ...).

A disk that runs out of room (QuotaExceededError, or a short write) keeps the new pieces in memory as before and now says
`nospace` on the channel.

**Page** (`dev.html`). `lost` messages, and a look every 10 s at whether the disks' directories are still in OPFS (the only
way to learn it on a profile where the open files outlive the removal), show one note over the bottom edge: the browser
removed this site's storage at HH:MM, files since then are not saved, closing or reloading the tab starts the machine from
scratch.

**Cache** (`jitcache.js`, header comment "Room in the browser's storage"). A budget from `estimate()` (a tenth of the quota
and a quarter of what is left; below 16 MiB the store is not kept, only used in memory, 128 MiB); a file made with more room
is cut down to what fits. Saving is done in place and only for what is new by a worker with a sync access handle (no swap
copy), the changed 64 KiB blocks of the index and the new records, records first so that a save cut short leaves a file the
next visit still reads. Before each save the storage is looked at again (never beyond 85% of the quota). Any error of the file
system, `NotFoundError` or `QuotaExceededError` included, ends the keeping for the visit with one line; `nospace` from a disk
removes the cache's file at once (the disks come first), `lost` closes it. The store goes on in memory either way: every
process of the machine still shares it.

## Tests

- `node tests/disk-worker-lost.js`, `node tests/disk-worker-pool.js` (a fake OPFS that Chrome-style "removes" itself), `node tests/jitcache-storage.js`: no browser.
- `tools/with-browser.sh WHO node tests/storage-chrome.js [--private]`: the cache in Chrome, with a real quota cap and a real removal.
- `tools/with-browser.sh WHO node tools/jitdesk.js DIST/www --private --quota-mb N --remove-storage-after 1 --overlay DIR`: the whole desktop in a private context, the storage removed after the first program has its window, the rest launched, the note looked for.
- `tools/storage-probe.js`: the table above.
