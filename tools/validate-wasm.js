// Validate the modules the JIT dumped (RV_JIT_DUMP=DIR), without booting anything:
//   node --experimental-wasm-memory64 tools/validate-wasm.js DIR
const fs = require("fs");
const path = require("path");

const dir = process.argv[2];
let bad = 0, total = 0;
for (const name of fs.readdirSync(dir).filter((n) => n.endsWith(".wasm")).sort()) {
  const bytes = fs.readFileSync(path.join(dir, name));
  total++;
  try {
    new WebAssembly.Module(bytes);
  } catch (error) {
    bad++;
    if (bad <= 5) console.log(name + ": " + error.message);
  }
}
console.log(total + " modules, " + bad + " invalid");
process.exit(bad ? 1 : 0);
