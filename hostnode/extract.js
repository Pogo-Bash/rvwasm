// Run by the Node.js of the guest system (node --expose-internals extract.js OUT_DIR): take out of it what
// hostnode is made from. Node's JavaScript library, one file per module, into OUT_DIR/lib (hostnode runs it
// unchanged), and into OUT_DIR/node.json what its C++ half holds as plain data: versions, the build's
// configuration, the defaults of the command line options, and for every internal binding its shape (which
// functions and classes it has) and its constants. The shape is what hostnode's own bindings are written and
// checked against; the constants are used as they are.
"use strict";
const fs = require("fs");
const path = require("path");
const { internalBinding } = require("internal/test/binding");
const { types } = require("util");

const out = process.argv[2];
const natives = process.binding("natives");
const ids = [];
for (const id of Object.keys(natives)) {
  if (typeof natives[id] !== "string") {
    continue;
  }
  const file = path.join(out, "lib", id + ".js");
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, natives[id]);
  ids.push(id);
}

// The HTTP parser (llhttp) as WebAssembly, which undici carries: hostnode's http_parser binding runs it
// (hostnode/js/binding/http.js). From undici's own file where the system has it, else out of the undici
// that is built into node.
{
  let base64 = null;
  try {
    base64 = require("/usr/share/nodejs/undici/lib/llhttp/llhttp-wasm.js").toString("base64");
  } catch (error) {
    const found = /AGFzbQEAAAAB[A-Za-z0-9+\/]{20000,}={0,2}/.exec(natives["internal/deps/undici/undici"] || "");
    base64 = found && found[0];
  }
  if (base64) {
    fs.writeFileSync(path.join(out, "llhttp.wasm"), Buffer.from(base64, "base64"));
  }
}

// The lexer that finds the imports in the text of an ES module (hostnode/js/binding/esm.js), if the system
// has it (Debian: node-es-module-lexer).
try {
  fs.copyFileSync("/usr/share/nodejs/es-module-lexer/dist/lexer.cjs", path.join(out, "es-module-lexer.cjs"));
} catch (error) {
  console.log("no es-module-lexer on this system: hostnode will not run ES modules");
}

const describe = (value, depth, seen) => {
  const type = typeof value;
  if (value === null || type === "number" || type === "string" || type === "boolean" || type === "undefined") {
    return type === "undefined" ? { $: "undefined" } : value;
  }
  if (type === "bigint") {
    return { $: "bigint", value: String(value) };
  }
  if (type === "symbol") {
    return { $: "symbol", description: value.description };
  }
  if (seen.has(value) || depth > 4) {
    return { $: "..." };
  }
  seen.add(value);
  if (ArrayBuffer.isView(value)) {
    return { $: value.constructor.name, length: value.length };
  }
  if (Array.isArray(value)) {
    return value.length > 2000 ? { $: "Array", length: value.length } : value.map((v) => describe(v, depth + 1, seen));
  }
  if (types.isMap(value)) {
    return { $: "Map", entries: [...Map.prototype.entries.call(value)].map(([k, v]) => [describe(k, depth + 1, seen), describe(v, depth + 1, seen)]) };
  }
  if (types.isSet(value)) {
    return { $: "Set", values: [...Set.prototype.values.call(value)].map((v) => describe(v, depth + 1, seen)) };
  }
  const result = {};
  if (type === "function") {
    result.$ = "function";
    result.length = value.length;
    const proto = value.prototype;
    if (proto && typeof proto === "object") {
      const methods = Object.getOwnPropertyNames(proto).filter((n) => n !== "constructor");
      const parent = Object.getPrototypeOf(proto);
      if (methods.length || (parent && parent !== Object.prototype)) {
        result.prototype = methods;
        if (parent && parent !== Object.prototype && parent.constructor) {
          result.extends = parent.constructor.name;
        }
      }
    }
  }
  for (const name of Object.getOwnPropertyNames(value)) {
    if (type === "function" && (name === "length" || name === "name" || name === "prototype" ||
                                name === "arguments" || name === "caller")) {
      continue;
    }
    const d = Object.getOwnPropertyDescriptor(value, name);
    result[name] = "value" in d ? describe(d.value, depth + 1, seen) : { $: "accessor" };
  }
  return result;
};

const names = [
  "async_wrap", "blob", "block_list", "buffer", "builtins", "cares_wrap", "config", "constants", "contextify",
  "credentials", "crypto", "encoding_binding", "errors", "fs", "fs_dir", "fs_event_wrap", "heap_utils", "http2",
  "http_parser", "icu", "inspector", "js_stream", "js_udp_wrap", "messaging", "module_wrap", "mksnapshot",
  "options", "os", "performance", "permission", "pipe_wrap", "process_methods", "process_wrap", "report",
  "serdes", "signal_wrap", "spawn_sync", "stream_pipe", "stream_wrap", "string_decoder", "symbols",
  "task_queue", "tcp_wrap", "timers", "tls_wrap", "trace_events", "tty_wrap", "types", "udp_wrap", "url",
  "util", "uv", "v8", "wasi", "wasm_web_api", "watchdog", "worker", "zlib",
];
const bindings = {};
for (const name of names) {
  try {
    const binding = internalBinding(name);
    const d = describe(binding, 0, new Set());
    if (name === "builtins") {
      delete d.natives;	// the library itself: in lib/
    }
    bindings[name] = d;
  } catch (error) {
    bindings[name] = { $: "error", message: String(error.message) };
  }
}

const options = internalBinding("options");
const data = {
  version: process.version,
  versions: process.versions,
  release: process.release,
  features: process.features,
  config: process.config,
  ids: ids,
  options: describe(options.getCLIOptionsValues(), 0, new Set()),
  optionsInfo: describe(options.getCLIOptionsInfo(), 0, new Set()),
  uvErrors: [...internalBinding("uv").getErrorMap()],
  bindings: bindings,
};
fs.writeFileSync(path.join(out, "node.json"), JSON.stringify(data));
console.log(process.version + ": " + ids.length + " modules, " + names.length + " bindings described");
