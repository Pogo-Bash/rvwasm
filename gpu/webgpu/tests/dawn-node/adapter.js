#!/usr/bin/env node
"use strict";
// VK-02 acceptance 1: Dawn's Node bindings give an adapter (and a device with every feature and limit, as
// rvgpu-worker.js asks for). Prints the adapter, its features and its limits.
//   ./node_modules/.bin/node adapter.js [dawn option ...]      e.g. backend=vulkan
const { create, globals } = require("webgpu");
Object.assign(globalThis, globals);
(async () => {
  const gpu = create(process.argv.slice(2));
  const adapter = await gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) { console.log("FAIL no adapter"); process.exit(1); }
  const i = adapter.info;
  console.log("adapter:", [i.vendor, i.architecture, i.device, i.description].filter(Boolean).join(" / "));
  const limits = {};
  for (const k in adapter.limits) limits[k] = adapter.limits[k];
  console.log("features (" + adapter.features.size + "):", [...adapter.features].join(" "));
  console.log("limits (" + Object.keys(limits).length + "):", JSON.stringify(limits));
  const device = await adapter.requestDevice({ requiredFeatures: [...adapter.features], requiredLimits: limits });
  console.log("device: ok, queue " + typeof device.queue.submit);
  device.destroy();
  console.log("PASS adapter");
})().catch((e) => { console.log("FAIL", e); process.exit(1); });
