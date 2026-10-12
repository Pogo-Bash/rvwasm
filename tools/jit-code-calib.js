// Committed wasm code for rv's region modules (RV_JIT_DUMP=DIR files), each compiled and instantiated alone.
//   node --experimental-wasm-memory64 [V8 flags] tools/jit-code-calib.js DIR [N]
const fs = require("fs"), path = require("path");
const dir = process.argv[2], max = +(process.argv[3] || 1e9);
const files = fs.readdirSync(dir).filter((f) => f.endsWith(".wasm")).slice(0, max);
const memory = new WebAssembly.Memory({ initial: 1, maximum: 65536, shared: true, index: "i64" });
const table = new WebAssembly.Table({ initial: 65536, element: "anyfunc" });
const keep = [];
let wire = 0;
const t0 = Date.now();
for (const f of files) {
  const bytes = fs.readFileSync(path.join(dir, f));
  wire += bytes.length;
  const module = new WebAssembly.Module(bytes);
  const imports = {};
  for (const item of WebAssembly.Module.imports(module)) {
    imports[item.module] = imports[item.module] || {};
    if (item.kind == "memory") imports[item.module][item.name] = memory;
    else if (item.kind == "table") imports[item.module][item.name] = table;
    else if (item.kind == "function") imports[item.module][item.name] = () => 0n;
    else throw new Error("import " + item.kind);
  }
  keep.push(new WebAssembly.Instance(module, imports));
}
const out = require("child_process").execSync(path.join(__dirname, "codemem.sh") + " " + process.pid).toString().trim().split(" ");
console.log(`${files.length} modules, ${(wire / 1048576).toFixed(1)} MiB wire, ${out[0]} MiB committed (${out[1]} ranges), ` +
  `${(out[0] * 1048576 / files.length / 1024).toFixed(1)} KiB a module, ${(out[0] * 1048576 / wire).toFixed(2)} x wire, ${Date.now() - t0} ms`);
// With WAIT=ms: look again after that long (background tier-up).
if (process.env.WAIT) setTimeout(() => {
  const o = require("child_process").execSync(path.join(__dirname, "codemem.sh") + " " + process.pid).toString().trim().split(" ");
  console.log(`after ${process.env.WAIT} ms: ${o[0]} MiB committed (${o[1]} ranges), ${(o[0] * 1048576 / files.length / 1024).toFixed(1)} KiB a module`);
}, +process.env.WAIT);
