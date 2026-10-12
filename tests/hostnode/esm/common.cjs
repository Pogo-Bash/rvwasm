exports.value = "from cjs";
exports.fromCjs = 7;
exports.load = () => import("./lib.mjs").then((m) => m.named);
