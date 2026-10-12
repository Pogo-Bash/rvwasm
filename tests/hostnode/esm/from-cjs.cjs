// import() from a CommonJS module.
import("./lib.mjs").then((m) => { console.log("import() in cjs", m.default, m.named); return require("./common.cjs").load(); }).then((n) => console.log("and again", n));
