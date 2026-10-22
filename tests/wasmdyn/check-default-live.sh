#!/bin/bash
# The native programs as the DEFAULT of a live site, from outside, without a browser: the page and the runtime the site serves carry the
# rule (dev.html asks rv_dl_wanted of linux.js; patch-runtime.py item 28), and that function, run on the served text, gives: on with no
# flag in Chrome, off with ?dl=0, on with ?dl=1 in Firefox, off in Firefox without a flag, and off outside a page (node-machine).
#   [URL=https://plasmaontheweb.com] tests/wasmdyn/check-default-live.sh
URL=${URL:-https://plasmaontheweb.com}
T=$(mktemp -d)
curl -sf -m 60 -o $T/dev.html "$URL/dev.html" || { echo "check-default-live: no $URL/dev.html" >&2; exit 1; }
curl -sf -m 120 -o $T/linux.js "$URL/linux.js" || { echo "check-default-live: no $URL/linux.js" >&2; exit 1; }
grep -q "const dl_wanted = typeof rv_dl_wanted" $T/dev.html && echo "ok   dev.html asks rv_dl_wanted" || { echo "FAIL dev.html has no dl_wanted"; exit 1; }
grep -q "if (disk && dl_wanted" $T/dev.html && echo "ok   dev.html declares the nqs disk by dl_wanted" || { echo "FAIL dev.html: disk"; exit 1; }
node - $T/linux.js <<'JS'
const src = require("fs").readFileSync(process.argv[2], "utf8");
const m = src.match(/const rv_dl_wanted = [\s\S]*?\n};\n/);
if (!m) { console.log("FAIL linux.js has no rv_dl_wanted"); process.exit(1); }
const f = new Function(m[0] + "; return rv_dl_wanted;")();
const ff = "Mozilla/5.0 (Macintosh) Gecko/20100101 Firefox/130.0", ch = "Mozilla/5.0 (Macintosh) AppleWebKit/537.36 Chrome/154.0 Safari/537.36";
let bad = 0;
const t = (q, ua, page, want, what) => { globalThis.document = page ? {} : undefined; if (!page) delete globalThis.document; const got = f(q, ua); console.log((got === want ? "ok   " : "FAIL ") + what + " -> " + got); if (got !== want) bad++; };
t("", ch, true, true, "page, Chrome, no flag: on");
t("?dl=0", ch, true, false, "page, Chrome, ?dl=0: off");
t("?dl=1", ch, true, true, "page, Chrome, ?dl=1: on");
t("", ff, true, false, "page, Firefox, no flag: off (the Firefox gate)");
t("?dl=1", ff, true, true, "page, Firefox, ?dl=1: on");
t("?x=1&dl=0", ch, true, false, "page, Chrome, other params and dl=0: off");
t("", ch, false, false, "no page (node-machine), no flag: off");
process.exit(bad ? 1 : 0);
JS
rc=$?
rm -rf $T
exit $rc
