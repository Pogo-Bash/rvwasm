function loop() { let s = 0; for (let i = 0; i < 3e7; i++) { s = (s + i * i) % 1000003; } return s; }
function objects() { let a = []; for (let i = 0; i < 4e5; i++) { a.push({ k: i, v: "x" + i }); } let m = new Map(); for (const o of a) { m.set(o.v, o.k); } return m.size; }
function strings() { let s = ""; for (let i = 0; i < 2e5; i++) { s += String.fromCharCode(65 + i % 26); } return s.split("A").join("b").length; }
function floats() { let x = 0; for (let i = 1; i < 1e7; i++) { x += Math.sqrt(i) * Math.sin(i) / i; } return x.toFixed(3); }
const now = () => (typeof performance != "undefined" ? performance.now() : Date.now());
const out = [];
for (const f of [loop, objects, strings, floats, loop]) { const t = now(); const r = f(); out.push(f.name + " " + Math.round(now() - t) + " ms"); }
console.log("JSBENCH " + out.join(", "));
