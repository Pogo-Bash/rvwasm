// ES modules: static and dynamic imports, import.meta, built-in and CommonJS modules seen from a module.
import { readFileSync } from "node:fs";
import path, { basename } from "node:path";
import { createRequire } from "node:module";
import defaultExport, { named, counter, bump } from "./lib.mjs";
import * as all from "./lib.mjs";
import cjs from "./common.cjs";
import { fromCjs } from "./common.cjs";
import data from "./data.json" with { type: "json" };
const require = createRequire(import.meta.url);
console.log("static", defaultExport, named, basename(import.meta.url), typeof readFileSync, path.sep, Object.keys(all).sort().join(","));
bump();
console.log("live binding", counter, all.counter);
console.log("cjs", cjs.value, fromCjs, require("./common.cjs") === cjs, data.answer);
console.log("meta", import.meta.filename === new URL(import.meta.url).pathname, import.meta.dirname === path.dirname(import.meta.filename), import.meta.resolve("./lib.mjs") === new URL("./lib.mjs", import.meta.url).href);
const dyn = await import("./lib.mjs");
console.log("dynamic", dyn === all, (await import("node:os")).EOL === "\n", (await import("./common.cjs")).default.value);
const s = "import('not real') import.meta";
console.log("strings untouched", s.length);
try { await import("./missing.mjs"); } catch (e) { console.log("missing", e.code); }
await new Promise((r) => setTimeout(r, 5));
console.log("top-level await done");
export {};
