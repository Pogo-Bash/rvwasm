// Web addresses from the machine, opened in the person's own browser (hostopen/ in the source tree has the
// machine's side: rv-host-open, and xdg-open and the like in front of Debian's).
//
// A program in the machine says an address on the BroadcastChannel "rv-host" with a mark it wrote into the
// machine's memory; every tab of this origin hears it, and only the page whose machine has that mark in its
// memory takes it (two tabs, two machines: each opens only its own machine's addresses). Then:
//
// - The address must be http or https, at most 2048 characters, without spaces or control characters, and
//   without a name or password in it (check() below; the machine's side checks the same, the page again).
//   javascript:, data:, file: and every other kind are refused: the guest is not to run script in this
//   origin, read the host's files, or start the host's programs.
// - A page may open a tab only right after the person clicked or pressed a key on it (browsers block other
//   window.open calls as popups). So if the person clicked on the screen in the last three seconds (a link in
//   a guest program, most likely, and its click is still the page's "user activation"), the address is
//   opened at once; otherwise it is shown in the screen's top right corner as a link the person clicks
//   ("Open on this computer: example.com/...", with a close button). Several waiting: the newest is shown, and
//   how many more. They are forgotten after two minutes.
// - Opened in a new tab without an opener (the tab cannot reach this page), as a link with rel="noopener
//   noreferrer" when the person clicks it.
//
// Files the other way (rv-host-save FILE, Dolphin's "Save to This Computer"): the machine's side reads the file
// into its own memory and says where, with a mark as above; this page copies it out of its machine's memory and
// hands it to the browser as a download (into the person's Downloads folder): at once after a recent click on
// the screen, else shown in the same corner ("Save to this computer: NAME"), to be clicked. At most 512 MiB.
//
// Exposed as window.rvHostOpen: check(url) -> [href, null] or [null, why]; ask(url, { direct }) (what the
// machine's requests come to; direct: may open at once if the page has a user activation now); waiting()
// (the addresses shown); log (the last 20 requests and what became of them).
(() => {
  "use strict";

  const MAX_URL = 2048;
  const MARK = 32;
  const CLICK_MS = 3000;	// a click on the screen this recent may open a tab at once
  const KEEP_MS = 120000;	// a request not taken is forgotten after this long
  const MAX_WAITING = 8;
  const MAX_SAVE = 512 * 1024 * 1024;

  const check = (raw) => {
    if (typeof raw !== "string" || !raw.length) {
      return [null, "no address"];
    }
    if (raw.length > MAX_URL) {
      return [null, "longer than " + MAX_URL + " characters"];
    }
    if (/[\u0000- \u007f-\u009f]/.test(raw)) {
      return [null, "spaces or control characters in it"];
    }
    let url;
    try {
      url = new URL(raw);
    } catch (error) {
      return [null, "not an address"];
    }
    if (url.protocol !== "http:" && url.protocol !== "https:") {
      return [null, "only http and https addresses are opened here (not " + url.protocol + ")"];
    }
    if (url.username || url.password) {
      return [null, "a name or password in the address"];
    }
    if (!url.hostname) {
      return [null, "no host in the address"];
    }
    return [url.href, null];
  };

  const log = [];
  const remember = (entry) => {
    log.push(entry);
    if (log.length > 20) {
      log.shift();
    }
    console.log("hostopen: " + entry.status + " " + entry.url + (entry.reason ? " (" + entry.reason + ")" : ""));
  };

  // ---- the person's last click on the screen (the stage: the screen and what is drawn over it)
  let lastClick = -Infinity;
  window.addEventListener("pointerdown", (event) => {
    const stage = document.getElementById("rv-stage") || document.getElementById("screen");
    if (stage && stage.contains(event.target) && !(box && box.contains(event.target))) {
      lastClick = performance.now();
    }
  }, true);

  // Opened now, if the browser lets this page open a tab now; true if it did.
  const openNow = (href) => {
    let w = null;
    try {
      w = window.open("about:blank", "_blank");
    } catch (error) {
      w = null;
    }
    if (!w) {
      return false;
    }
    try {
      w.opener = null;	// (the new tab cannot reach this page)
    } catch (error) {
      // (nothing: it is a new about:blank of this origin, so this does not fail)
    }
    w.location.href = href;
    return true;
  };

  // ---- the prompt in the top right corner of the screen
  let box = null, link = null, more = null, boxLabel = null;
  const forget = (w, status) => {
    remember({ url: w.save ? w.name : w.href, status, at: Date.now() });
    if (w.save) {
      URL.revokeObjectURL(w.href);
    }
  };
  const waiting = [];	// [{ href, at }], the newest last

  const setup = () => {
    const css = document.createElement("link");
    css.rel = "stylesheet";
    css.href = "hostopen.css";
    document.head.appendChild(css);
    box = document.createElement("div");
    box.id = "rv-hostopen";
    box.style.display = "none";
    const label = document.createElement("span");
    label.className = "rv-hostopen-label";
    label.textContent = "Open on this computer:";
    link = document.createElement("a");
    link.className = "rv-hostopen-link";
    link.target = "_blank";
    link.rel = "noopener noreferrer";
    // (No focus for the link, so that the keys stay with the screen.)
    link.addEventListener("mousedown", (event) => event.preventDefault());
    link.addEventListener("click", () => {
      const w = waiting.pop();
      if (w) {
        remember({ url: w.save ? w.name : w.href, status: w.save ? "saved" : "opened", reason: "clicked", at: Date.now() });
        if (w.save) {
          setTimeout(() => URL.revokeObjectURL(w.href), 60000);
        }
      }
      setTimeout(show, 0);
      const sc = document.getElementById("screen");
      if (sc) {
        sc.focus({ preventScroll: true });
      }
    });
    more = document.createElement("span");
    more.className = "rv-hostopen-more";
    const close = document.createElement("button");
    close.className = "rv-hostopen-close";
    close.textContent = "×";
    close.title = "Not now";
    close.tabIndex = -1;
    close.addEventListener("mousedown", (event) => event.preventDefault());
    close.addEventListener("click", () => {
      const w = waiting.pop();
      if (w) {
        forget(w, "dismissed");
      }
      show();
    });
    box.append(label, link, more, close);
    boxLabel = label;
    (document.getElementById("rv-stage") || document.body).appendChild(box);
  };

  // What the box shows: the newest address waiting, or nothing.
  const show = () => {
    if (!box) {
      return;
    }
    const now = performance.now();
    while (waiting.length && now - waiting[0].at > KEEP_MS) {
      forget(waiting.shift(), "expired");
    }
    const w = waiting[waiting.length - 1];
    if (!w) {
      box.style.display = "none";
      return;
    }
    more.textContent = waiting.length > 1 ? "+" + (waiting.length - 1) : "";
    if (w.save) {
      boxLabel.textContent = "Save to this computer:";
      link.href = w.href;
      link.download = w.name;
      link.title = w.name + " (" + w.size + " bytes): into this computer's downloads";
      link.textContent = "";
      const name = document.createElement("b");
      name.textContent = w.name.length > 48 ? w.name.slice(0, 47) + "…" : w.name;
      link.append(name, document.createTextNode(" " + sizeText(w.size)));
      box.style.display = "";
      return;
    }
    boxLabel.textContent = "Open on this computer:";
    link.removeAttribute("download");
    const url = new URL(w.href);
    let rest = url.pathname + url.search + url.hash;
    if (rest === "/") {
      rest = "";
    }
    if (rest.length > 48) {
      rest = rest.slice(0, 47) + "…";
    }
    link.href = w.href;
    link.title = w.href;
    link.textContent = "";
    const host = document.createElement("b");
    host.textContent = url.host;	// (the host as the browser has it: an international name in its ASCII form)
    link.append(host, document.createTextNode(rest));
    more.textContent = waiting.length > 1 ? "+" + (waiting.length - 1) : "";
    box.style.display = "";
  };
  setInterval(() => {
    if (waiting.length) {
      show();
    }
  }, 10000);

  // An address to open: at once if it may be, else shown. "opened", "asked" or "refused" (with the reason).
  const ask = (raw, { direct = false } = {}) => {
    const [href, why] = check(raw);
    if (!href) {
      remember({ url: String(raw).slice(0, 200), status: "refused", reason: why, at: Date.now() });
      return { status: "refused", reason: why };
    }
    const activation = !navigator.userActivation || navigator.userActivation.isActive;
    if ((direct || performance.now() - lastClick < CLICK_MS) && activation && openNow(href)) {
      remember({ url: href, status: "opened", reason: "at once", at: Date.now() });
      return { status: "opened" };
    }
    waiting.push({ href, at: performance.now() });
    if (waiting.length > MAX_WAITING) {
      forget(waiting.shift(), "dropped");
    }
    remember({ url: href, status: "asked", at: Date.now() });
    show();
    return { status: "asked" };
  };

  const sizeText = (n) => n < 1024 ? n + " bytes" : n < 1 << 20 ? (n / 1024).toFixed(1) + " KiB" : (n / 1048576).toFixed(1) + " MiB";

  // A file from the machine: a download now if the person has just clicked on the screen (a page may start one
  // then without the browser asking), else waiting in the corner. "saved", "asked" or "refused".
  const saveFile = (name, bytes) => {
    name = String(name).replace(/[\u0000-\u001f\u007f/\\]/g, "_").slice(0, 255) || "file";
    const href = URL.createObjectURL(new Blob([bytes]));
    const activation = !navigator.userActivation || navigator.userActivation.isActive;
    if (performance.now() - lastClick < CLICK_MS && activation) {
      const a = document.createElement("a");
      a.href = href;
      a.download = name;
      a.rel = "noopener";
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(href), 60000);
      remember({ url: name, status: "saved", reason: "at once", at: Date.now() });
      return { status: "saved" };
    }
    waiting.push({ href, name, size: bytes.length, save: true, at: performance.now() });
    if (waiting.length > MAX_WAITING) {
      forget(waiting.shift(), "dropped");
    }
    remember({ url: name, status: "asked", at: Date.now() });
    show();
    return { status: "asked" };
  };

  // ---- requests from the machine
  // Ours? The mark has to be in our machine's memory where the message says.
  const ours = (m) => {
    const os = window.linuxOs;
    if (!os || !os.memory || m.markBytes.length !== MARK || m.markBytes[0] !== 0x52 || m.markBytes[1] !== 0x56) {
      return false;
    }
    const mark = Number(m.mark);
    const buffer = os.memory.buffer;
    if (!Number.isSafeInteger(mark) || mark < 0 || mark + MARK > buffer.byteLength) {
      return false;
    }
    const there = new Uint8Array(buffer, mark, MARK);
    for (let i = 0; i < MARK; i++) {
      if (there[i] !== m.markBytes[i]) {
        return false;	// (another tab's machine)
      }
    }
    return true;
  };
  const channel = window.rvChannel("rv-host");
  channel.onmessage = (event) => {
    const m = event.data;
    if (m && m.type === "save" && typeof m.id === "string" && Array.isArray(m.markBytes) && ours(m)) {
      const buffer = window.linuxOs.memory.buffer, at = Number(m.address), size = Number(m.size);
      let result;
      if (!Number.isSafeInteger(at) || !Number.isSafeInteger(size) || at < 0 || size < 0 || size > MAX_SAVE
          || at + size > buffer.byteLength) {
        result = { status: "refused", reason: "not a file this page can take (size " + m.size + ")" };
      } else {
        result = saveFile(m.name, new Uint8Array(buffer, at, size).slice());	// (a copy: Blobs take no shared memory)
      }
      channel.postMessage({ type: "save-answer", id: m.id, status: result.status, reason: result.reason || "" });
      return;
    }
    if (!m || m.type !== "open" || typeof m.id !== "string" || !Array.isArray(m.markBytes)) {
      return;
    }
    if (!ours(m)) {
      return;
    }
    const result = ask(m.url);
    channel.postMessage({ type: "open-answer", id: m.id, status: result.status, reason: result.reason || "" });
  };

  window.rvHostOpen = { check, ask, saveFile, log, waiting: () => waiting.map((w) => (w.save ? "save:" + w.name : w.href)) };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", setup);
  } else {
    setup();
  }
})();
