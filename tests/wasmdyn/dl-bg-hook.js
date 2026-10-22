// node-machine --hook: tests/wasmdyn/widgets-hook.js (the window server's table and the module cache's statistics) plus, with
// DLBG_URL set to the library manifest's URL, the page's background compile (tools/www/dev.html, the text between BEGIN dl-background
// and END dl-background) started DLBG_DELAY seconds (default 0) after the machine is up. There is no page here, so the display is a
// mock: the desktop is "up" (one window) and nothing is drawn; the job still pauses while a Worker loads libraries (a program
// starting), as in the page. DLBG_KEEP: libraries with an "order" below it are kept as Modules (default: all), the others warmed.
// Every library it compiles is a line "DLBG: key name ms" on stderr, the end "DLBG-DONE: n libraries".
const fs = require("fs");
const path = require("path");
const widgets = require("./widgets-hook.js");
module.exports = (os, info) => {
  const hook = widgets(os, info);
  const url = process.env.DLBG_URL;
  if (url) {
    const html = fs.readFileSync(path.join(__dirname, "../../tools/www/dev.html"), "utf8");
    const text = html.slice(html.indexOf("// BEGIN dl-background"), html.indexOf("// END dl-background"));
    const { dlBackground } = new Function(text + "\nreturn { dlBackground };")();
    const keep = process.env.DLBG_KEEP === undefined ? Infinity : Number(process.env.DLBG_KEEP);
    setTimeout(() => {
      const t0 = Date.now();
      const wrapped = { dl: {
        ...os.dl,
        log: os.dl.log,
        keys: () => os.dl.keys(),
        preload: async (entries) => { const t = Date.now(); await os.dl.preload(entries); console.error("DLBG: keep " + entries[0].key + " " + (Date.now() - t) + " ms"); },
        warm: async (entry) => { const t = Date.now(); await os.dl.warm(entry); console.error("DLBG: warm " + entry.key + " " + (Date.now() - t) + " ms"); },
      } };
      dlBackground(wrapped, url, {
        base: url,
        fetchJson: async (u) => (await fetch(u)).json(),
        sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
        now: () => Date.now(),
        hidden: () => false,
        display: () => ({ bytes: 0, windows: 1 }),
        keepBelow: keep,
        log: (m) => console.error(m),
      }).then((r) => console.error("DLBG-DONE: " + r.done + " libraries, " + r.kept + " kept, " + ((Date.now() - t0) / 1000).toFixed(1) + " s"),
              (e) => console.error("DLBG-FAILED: " + e.message));
    }, Number(process.env.DLBG_DELAY || 0) * 1000);
  }
  return hook;
};
