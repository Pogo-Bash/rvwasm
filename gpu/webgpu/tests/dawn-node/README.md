# dawn-node: rvgpu on Dawn's Node bindings, no browser (VK-02)

`webgpu` (npm, Dawn's `dawn.node`, prebuilt) gives Node a real WebGPU. On the Mac that is Metal. Node 26
(npm package `node`, pinned here) is needed because rvgpu.wasm uses JSPI; Node 22 has none.

    cd gpu/webgpu/tests/dawn-node && npm ci
    ./node_modules/.bin/node adapter.js               # adapter name, features, limits, device
    ./node_modules/.bin/node smoke.js [DIR] clear|draw  # rvgpu.wasm smoke tests in a Node Worker

`smoke.js` runs `tools/www/rvgpu-worker.js` unmodified inside `worker-shim.js` (a worker_threads Worker with the
browser-Worker surface it needs: `navigator.gpu`, `importScripts`, `fetch` of local files, `WorkerGlobalScope`, no
`process`, a stand-in canvas). DIR defaults to `build/rvgpu-page` (rvgpu.js, rvgpu.wasm from
`gpu/webgpu/build/smoke-page.sh` or another worktree). The ring (`start` message) path is VK-06's.
Dawn version: whatever `webgpu` 0.6.2 bundles (newer than build/dawn.pin c943ea32; the JS API is the
same standard WebGPU rvgpu.js uses).
