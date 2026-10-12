// Guest players' video, decoded by this browser (docs/video-decode.md): starts the decoder's Worker
// (vdec-worker.js) on gpuserver's announcement ("rv-gpu", whose `vdec` is the vdec ring), hands it the machine's
// memory, and joins it to the GPU executor's Worker (window.rvGpu.vdecPort) so that frames can be drawn into
// guest surfaces without passing through the machine (zero copy). Without an executor that draws (no GPU, or
// ?exec=webgpu) the guest's players copy frames (vaapi-copy).
//   ?vdec=off       no decoder: guest players decode in software
//   ?vdec=exact     zero copy draws the frame's own Y and CbCr bytes (copyTo + upload) instead of a GPU round
//                   trip through RGB, which is not bit-exact
//   ?vdecframes=MiB frames kept for later copies, at most (384)
// window.rvVdec: { caps, stats, held, gpu } for the console.
(() => {
  "use strict";
  const params = new URLSearchParams(location.search);
  const mode = params.get("vdec") || "on";
  const rvVdec = window.rvVdec = { caps: null, stats: null, gpu: false, started: false, log: [] };
  if (mode === "off") {
    return;
  }
  const base = (document.currentScript ? document.currentScript.src : "vdec.js").replace(/[^/]*$/, "");
  let started = false;
  const start = (memory, ring) => {
    started = true;
    const worker = new Worker(base + "vdec-worker.js");
    const channel = new MessageChannel();
    const gpu = !!(window.rvGpu && window.rvGpu.vdecPort && window.rvGpu.vdecPort(channel.port2));
    worker.onmessage = (event) => {
      const m = event.data;
      if (m.type === "log") {
        console.log(m.text);
        rvVdec.log.push(m.text);
        if (rvVdec.log.length > 50) {
          rvVdec.log.shift();
        }
      } else if (m.type === "ready") {
        rvVdec.caps = m.caps;
        rvVdec.gpu = m.gpu;
        console.log("vdec: the page decodes " + (m.caps.map((c) => ["", "H.264", "HEVC", "VP8", "VP9", "AV1"][c.codec]
          + " " + c.max_w + "x" + c.max_h + (c.flags & 1 ? " (hardware)" : " (software)")).join(", ") || "nothing")
          + "; frames " + (m.gpu ? "drawn into GPU surfaces" : "copied") + " (" + m.ms + " ms to ask)");
      } else if (m.type === "stats") {
        rvVdec.stats = m.stats;
        rvVdec.held = m.held;
        rvVdec.heldMiB = m.heldMiB;
      }
    };
    worker.postMessage({ type: "start", memory, ring, port: gpu ? channel.port1 : null, exact: mode === "exact",
                         frames: Number(params.get("vdecframes")) || 0 }, gpu ? [channel.port1] : []);
    rvVdec.started = true;
  };
  // (gpuserver's announcements, on this page load's channel: tabscope's "rv-gpu-ID", display.js's window.rvChannel)
  const channel = window.rvChannel ? window.rvChannel("rv-gpu") : new BroadcastChannel("rv-gpu");
  let first = 0;
  channel.onmessage = (event) => {
    const m = event.data;
    const os = window.linuxOs;
    if (started || !m || m.type !== "gpu" || !m.vdec || !os || !os.memory) {
      return;
    }
    // (the executor starts on the same announcement: give it a moment to have its Worker, then join them)
    first = first || performance.now();
    const g = window.rvGpu && window.rvGpu.stats;
    if (g && !g.exec_kind && performance.now() - first < 5000 && params.get("gpu") !== "off") {
      return;
    }
    start(os.memory, m.vdec);
  };
  setInterval(() => started || channel.postMessage({ type: "gpu?" }), 1000);
})();
