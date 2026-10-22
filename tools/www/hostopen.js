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
// - Nothing opens by itself (owner, 2026-10-14): every address is shown in a prompt that is the page's, not the
//   machine's screen (a fixed box at the top of the page, over the bar's row, drawn by the page in the
//   landing page's look: Catppuccin Mocha, JetBrains Mono), "Open example.com/... in a new tab?", with
//   the full address under it (the host in colour) and Open / Cancel. The click on Open is the user activation
//   the browser wants for a new tab. Several waiting: the newest is shown first, with a count; they are
//   forgotten after two minutes. Enter opens and Escape cancels while the prompt has the keyboard (click it or
//   Tab to it: it never takes the keys by itself, so a program in the machine cannot have the person's typing
//   open a tab).
// - Opened in a new tab without an opener (the tab cannot reach this page) and without a referrer: a link with
//   rel="noopener noreferrer" that Open clicks.
//
// Files the other way (rv-host-save FILE, Dolphin's "Save to This Computer"): the machine's side reads the file
// into its own memory and says where, with a mark as above; this page copies it out of its machine's memory and
// hands it to the browser as a download (into the person's Downloads folder): at once after a recent click on
// the screen (the menu entry that asked), else asked for in the same prompt ("Save NAME to this computer?").
// At most 512 MiB.
//
// Exposed as window.rvHostOpen: check(url) -> [href, null] or [null, why]; ask(url) (what the machine's
// requests come to: always shown in the prompt; { direct } is ignored); saveFile(name, bytes); waiting() (what
// the prompt holds, the newest last); log (the last 20 requests and what became of them).
(() => {
  "use strict";

  const MAX_URL = 2048;
  const MARK = 32;
  const CLICK_MS = 3000;	// a click on the screen this recent may start a download at once (a tab never opens so)
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
    if (stage && stage.contains(event.target)) {
      lastClick = performance.now();
    }
  }, true);

  // Opened as a link click, which is what carries the Open button's user activation into the new tab; rel
  // noopener noreferrer: no opener, no referrer. Must run inside the click (or key) handler.
  const openTab = (href) => {
    const a = document.createElement("a");
    a.href = href;
    a.target = "_blank";
    a.rel = "noopener noreferrer";
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    a.remove();
  };

  const download = (href, name) => {
    const a = document.createElement("a");
    a.href = href;
    a.download = name;
    a.rel = "noopener";
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    a.remove();
  };

  // ---- the prompt: the page's own box at the top of the page, not part of the machine's screen
  let box = null, title = null, titleHost = null, titleRest = null, titleTail = null, full = null, count = null, openButton = null, cancelButton = null;
  const waiting = [];	// [{ href, at, ... }], the newest last
  const forget = (w, status) => {
    remember({ url: w.save ? w.name : w.href, status, at: Date.now() });
    if (w.save) {
      URL.revokeObjectURL(w.href);
    }
  };

  const giveBack = () => {
    // (the keys go back to the machine's screen, as they were)
    const sc = document.getElementById("screen");
    if (sc) {
      sc.focus({ preventScroll: true });
    }
  };

  const accept = () => {
    const w = waiting.pop();
    if (!w) {
      return;
    }
    if (w.save) {
      download(w.href, w.name);
      remember({ url: w.name, status: "saved", reason: "confirmed", at: Date.now() });
      setTimeout(() => URL.revokeObjectURL(w.href), 60000);
    } else {
      openTab(w.href);
      remember({ url: w.href, status: "opened", reason: "confirmed", at: Date.now() });
    }
    show();
    giveBack();
  };
  const decline = () => {
    const w = waiting.pop();
    if (w) {
      forget(w, "cancelled");
    }
    show();
    giveBack();
  };

  const STYLE_ID = "rv-hostopen-style";
  const setup = () => {
    if (!document.getElementById(STYLE_ID)) {
      const css = document.createElement("link");
      css.id = STYLE_ID;
      css.rel = "stylesheet";
      css.href = "hostopen.css";
      document.head.appendChild(css);
    }
    box = document.createElement("div");
    box.id = "rv-hostopen";
    box.setAttribute("role", "alertdialog");
    box.setAttribute("aria-live", "polite");
    box.setAttribute("aria-label", "A program in the machine asks to use your browser");
    box.tabIndex = -1;
    box.style.display = "none";
    title = document.createElement("div");
    title.className = "rv-ho-title";
    // (built from text nodes only, never HTML: the address comes from the machine)
    const lead = document.createElement("span");
    lead.className = "rv-ho-dim";
    titleHost = document.createElement("b");
    titleRest = document.createElement("span");
    titleRest.className = "rv-ho-dim";
    titleTail = document.createElement("span");
    titleTail.className = "rv-ho-dim";
    title.append(lead, titleHost, titleRest, titleTail);
    title.lead = lead;
    full = document.createElement("div");
    full.className = "rv-ho-full";
    count = document.createElement("span");
    count.className = "rv-ho-count";
    const actions = document.createElement("div");
    actions.className = "rv-ho-actions";
    cancelButton = document.createElement("button");
    cancelButton.type = "button";
    cancelButton.className = "rv-ho-btn";
    cancelButton.textContent = "Cancel";
    cancelButton.addEventListener("click", decline);
    openButton = document.createElement("button");
    openButton.type = "button";
    openButton.className = "rv-ho-btn rv-ho-primary";
    openButton.textContent = "Open";
    openButton.addEventListener("click", accept);
    actions.append(count, cancelButton, openButton);
    box.append(title, full, actions);
    // Keys while the prompt has the keyboard: Escape cancels, Enter opens (a focused button does its own).
    box.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        decline();
      } else if (event.key === "Enter" && event.target === box) {
        event.preventDefault();
        event.stopPropagation();
        accept();
      }
    });
    // Keys typed into the prompt are the page's: the machine's screen does not get them.
    for (const type of ["keyup", "keypress"]) {
      box.addEventListener(type, (event) => event.stopPropagation());
    }
    document.body.appendChild(box);
    // A box in the body is not drawn while the stage is fullscreen (only the fullscreen element and what is in it is): it goes along.
    document.addEventListener("fullscreenchange", () => {
      (document.fullscreenElement || document.body).appendChild(box);
    });
  };

  const sizeText = (n) => n < 1024 ? n + " bytes" : n < 1 << 20 ? (n / 1024).toFixed(1) + " KiB" : (n / 1048576).toFixed(1) + " MiB";
  const clip = (text, n) => text.length > n ? text.slice(0, n - 1) + "…" : text;

  // What the prompt shows: the newest address waiting, or nothing.
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
    count.textContent = waiting.length > 1 ? waiting.length + " waiting, newest first" : "";
    full.textContent = "";
    if (w.save) {
      title.lead.textContent = "Save ";
      titleHost.textContent = clip(w.name, 56);
      titleRest.textContent = " (" + sizeText(w.size) + ")";
      titleTail.textContent = " to this computer?";
      full.append("Into this computer's downloads folder, as ", Object.assign(document.createElement("b"), { textContent: w.name }), ".");
      openButton.textContent = "Save";
      box.dataset.kind = "save";
    } else {
      const url = new URL(w.href);	// (the host as the browser has it: an international name in its ASCII form, so look-alike letters show)
      let rest = url.pathname + url.search + url.hash;
      if (rest === "/") {
        rest = "";
      }
      title.lead.textContent = "Open ";
      titleHost.textContent = url.host;
      titleRest.textContent = clip(rest, 48);
      titleTail.textContent = " in a new tab?";
      const scheme = document.createElement("span");
      scheme.className = "rv-ho-dim";
      scheme.textContent = url.protocol + "//";
      const host = document.createElement("b");
      host.textContent = url.host;
      full.append(scheme, host, rest);
      openButton.textContent = "Open";
      box.dataset.kind = "open";
    }
    box.style.display = "";
  };
  setInterval(() => {
    if (waiting.length) {
      show();
    }
  }, 10000);

  // An address to open: always asked of the person. "asked" or "refused" (with the reason). ({ direct } was how a
  // recent click used to open a tab at once; nothing opens without the prompt now.)
  const ask = (raw) => {
    const [href, why] = check(raw);
    if (!href) {
      remember({ url: String(raw).slice(0, 200), status: "refused", reason: why, at: Date.now() });
      return { status: "refused", reason: why };
    }
    waiting.push({ href, at: performance.now() });
    if (waiting.length > MAX_WAITING) {
      forget(waiting.shift(), "dropped");
    }
    remember({ url: href, status: "asked", at: Date.now() });
    show();
    return { status: "asked" };
  };

  // A file from the machine: a download now if the person has just clicked on the screen (a page may start one
  // then without the browser asking; the menu entry that asked is such a click), else asked of the person in the prompt. "saved" or "asked".
  const saveFile = (name, bytes) => {
    name = String(name).replace(/[\u0000-\u001f\u007f/\\]/g, "_").slice(0, 255) || "file";
    const href = URL.createObjectURL(new Blob([bytes]));
    const activation = !navigator.userActivation || navigator.userActivation.isActive;
    if (performance.now() - lastClick < CLICK_MS && activation) {
      download(href, name);
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
