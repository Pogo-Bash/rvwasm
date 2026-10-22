# Hardware video decoding: guest players on the page's WebCodecs

Branch `vdec` (from `gpuplus`), 2026-10-11. The design, why, what is built and tested, what is not.
Manual browser checks: play the clips in `gpu/vdec/test/` through the desktop (`tools/x/rv-video-check`).

## The picture

    guest player under rv (mpv; anything on FFmpeg's VA-API hwaccel: ffmpeg, Celluloid, VLC, Qt Multimedia; GStreamer's va)
      Debian's libva 2.22 -> LIBVA_DRIVER_NAME=rvwasm -> rvwasm_drv_video.so (ours, riscv64; gpu/vdec/driver)
        VA's stateless buffers -> one stateful chunk per picture:
          H.264  Annex B, SPS/PPS made from VA's parameters
          HEVC   Annex B, VPS/SPS/PPS made, slice headers with an RPS written again
          VP9    the frame (a hidden one followed by a show_existing_frame of ours)
          VP8    the frame with its tag put back
          AV1    a temporal unit: sequence header, frame header written from VA's values, tile groups
          | gpuserver's Unix socket ($XDG_RUNTIME_DIR/rvgpu-0), commands VDEC_* (0x10010..0x10019);
          | chunks and images in memory files gpuserver makes and both map
    gpuserver (gpu/server/vdec.js beside server.js): the "vdec ring", a second ring in its memory
          | records OPEN, MEM, STREAM, SURFACE, DECODE, COPY, EXPORT, SLOT_END, CLOSE
          | the page writes the client's status file (decoded, drawn, copied, bytes taken): the driver reads it
    page: tools/www/vdec-worker.js, a dedicated Worker with WebCodecs' VideoDecoder ("prefer-hardware":
          VideoToolbox on the Mac where Chrome has it), started and joined to the GPU executor by tools/www/vdec.js
        copy:      VideoFrame.copyTo straight into the guest's NV12 image in the machine's memory (vaGetImage)
        zero copy: the VideoFrame (transferred, a handle) to the GPU executor's Worker, which draws it into the
                   surface's two GPU resources (R8 luma, R8G8 chroma); the player's GL samples them through
                   EGL dmabuf import (vaExportSurfaceHandle, DRM_PRIME_2, separate layers R8 + GR88)

## Decision 1: VA-API, a driver of our own (not V4L2)

### What Debian trixie riscv64's players use

| app | VA-API | V4L2 stateful (m2m) |
| --- | --- | --- |
| mpv 0.40 | `--hwdec=vaapi` with EGL dmabuf interop (`video/out/hwdec/hwdec_vaapi.c`: `vaExportSurfaceHandle(DRM_PRIME_2, SEPARATE_LAYERS)` then `vaSyncSurface`), `vaapi-copy` | `v4l2m2m-copy` only |
| FFmpeg 7.1 | `-hwaccel vaapi`: H.264, HEVC, VP8, VP9, AV1 | `h264/hevc/vp8/vp9_v4l2m2m` (`allcodecs.c`); software output formats only (`v4l2_m2m_dec.c` has no hw configs); **no AV1** |
| GStreamer 1.26 | `va` plugin (`vah264dec` ... `vaav1dec`; a driver not on its list needs `GST_VA_ALL_DRIVERS=1`, which `desktop` sets) | `v4l2h264dec` etc., no stateful AV1 |
| VLC 3.0, Qt 6 Multimedia, Firefox ESR 140 | through FFmpeg's VA-API hwaccel (Firefox: gated by its probe, untested) | VLC/Qt: none; Firefox: needs a DRM-PRIME FFmpeg, not Debian's |
| Chromium | not built for riscv64 by Debian | |

So VA-API reaches every player with zero copy; V4L2 m2m gives mpv/FFmpeg copies only and no AV1 at all.
**VA-API is the front end.** The page side takes plain stateful chunks, so a V4L2 front end (a /dev/videoN
emulated in rv as the render node is) can be added on the same page backend later if GStreamer's v4l2
elements are wanted; nothing in Debian's players needs it.

### The catch: VA is stateless, WebCodecs is stateful, and how each codec is made whole

WebCodecs takes a bitstream with its headers (registrations: AVC/HEVC without `description` are Annex B, a
key chunk carries its parameter sets; AV1 chunks are temporal units). VA gives parsed values plus slice data.
What VA's slice data is (FFmpeg 7.1): H.264 and HEVC the whole NAL unit with emulation prevention bytes,
no start code (`h264dec.c`, `hevcdec.c`: `decode_slice(nal->raw_data)`); VP9 the whole frame, superframes
split, show_existing never reaching VA; VP8 the frame without its first 3 (10 on key frames) bytes
(`vaapi_vp8.c`); AV1 only each tile group's payload with its tile sizes (`av1dec.c`).

- **H.264** (`gpu/vdec/driver/h264.c`): SPS 0 and PPS from VA's fields (every field the slice header's syntax
  and the decoding depend on is there). High profile for all (a superset of what VA can carry), the smallest
  level whose MaxFS/MaxDpbMbs hold the picture and its references (Table A-1), no cropping (VA surfaces are
  whole macroblocks; the player crops), scaling lists from the IQ buffer (raster in VA, zig-zag in the
  bitstream) unless flat, `num_ref_idx_lX_default_active_minus1` read from a slice header that does not
  override them (as nvidia-vaapi-driver does). VUI: BT.709 full range, and **`max_num_reorder_frames` 0**:
  Chrome's decoders (`media/gpu/h264_decoder.cc`, used by `video_toolbox_video_decoder.cc` for every codec)
  then give each picture as soon as it is decoded, out of order or not (only a DVLOG), which is what VA
  needs (the player waits in vaSyncSurface for that picture; the chunk's timestamp names its surface, and
  WebCodecs passes timestamps through to frames). Key chunks carry SPS and every PPS. Not done: POC type 1
  (its offsets are not in VA), fields/MBAFF (Chrome's decoder has none either), not 4:2:0 8-bit.
- **HEVC** (`hevc.c`): VPS/SPS/PPS from VA's fields, seven sub-layers declared, no reordering, scaling lists
  (raster in VA per FFmpeg's `hevc/ps.c`, up-right diagonal in the bitstream). VA lacks the SPS's short-term
  RPS candidates and long-term pictures, so every slice segment header with an RPS is rewritten with it
  explicit (`short_term_ref_pic_set_sps_flag` 0, `st_ref_pic_set(0)`, long-term entries with their MSB) from
  VA's `ReferenceFrames` (FFmpeg lists exactly the pictures of the current RPS, Curr and Foll); the header is
  parsed to its end, the rest copied bit for bit, the slice data byte for byte with its emulation prevention
  bytes (the rewritten header ends in a non-zero byte, so they stay right, and so do entry points). The
  order is the spec's for an RPS, so the reference lists come out the same. Precedent: Mesa's VA frontend
  re-parses HEVC slice headers. Not done: range extensions, a long-term order needing a falling MSB cycle.
- **VP9** (`vp9.c`): the frame as given. A hidden frame (`show_frame` 0) gives no output, but the player
  shows its surface later; so it is followed by a 1-byte frame of ours, `show_existing_frame` of a slot it
  refreshed (read from its uncompressed header), whose output is its picture. libvpx does not count such a
  frame for the next frame's `use_prev_frame_mvs`.
- **VP8** (`vp8.c`): the frame tag rebuilt (`first_part_size = partition_size[0] + ceil(macroblock_offset/8)`,
  `show_frame` 1, as nvidia-vaapi-driver's `vp8.c`); GStreamer's buffer has the frame whole and goes as is.
- **AV1** (`av1.c`): sequence and frame headers written from `VADecPictureParameterBufferAV1` (CBS's values,
  inferred ones filled in), tile group OBUs around VA's tile data. One value is not in VA:
  `refresh_frame_flags`. It is not needed: the decoder's slots are ours. Each frame goes into one of our
  slots whose picture the stream no longer keeps (VA's `ref_frame_map` says what it keeps), and each frame's
  `ref_frame_idx` is written to name our slots holding the pictures VA names; what a slot carries (order hint,
  CDFs, loop filter and segmentation state, global motion, film grain) travels with it. Global motion is coded
  against our slot's saved parameters (subexponential coding, 5.9.24-27). A showable hidden frame is followed
  by a `show_existing_frame` of ours. With film grain FFmpeg decodes into a second surface; the picture goes
  to `current_display_picture`. Choices that do not change the decoding: no frame ids, explicit sizes, all
  loop filter deltas sent, `update_grain` 1, the sequence's enable_* flags on where VA says per frame what is
  used. Not done: 10-bit, a hidden key frame shown later, eight distinct live pictures plus a ninth (one has
  to go; a later reference to it fails).

Prior art: no VA-to-stateful shim exists; nvidia-vaapi-driver feeds NVDEC parameters (it rebuilds only the
VP8 tag), libva-v4l2-request maps VA to stateless V4L2.

## Decision 2: frames

### Zero copy (vaExportSurfaceHandle)

A surface's GPU resources are made on libva's own open of the render node (`DRM_IOCTL_VIRTGPU_RESOURCE_CREATE`
on `drm_state->fd`, so `RVGPU_DRM=1`): luma `R8_UNORM` w×h, chroma `R8G8_UNORM` w/2×h/2, textures in the
page's executor; their backing files are 4 KiB stand-ins (the pixels never live in the machine; the file is
the dmabuf's identity). They are made at the first export of a surface; from then on every picture decoded
into it is drawn into them. `vaExportSurfaceHandle(DRM_PRIME_2)`: separate layers, `DRM_FORMAT_R8` and
`DRM_FORMAT_GR88`, LINEAR (mpv checks only the plane count); composed layers (one NV12 layer, for
`vo=dmabuf-wayland`) are refused, since the window server cannot show NV12 buffers.

The player's Mesa (virgl DRM winsys) imports the planes with `PRIME_FD_TO_HANDLE` on *its own* open of the
render node. rv now imports across opens (rv/virtgpu.c, `prime_import`): `VCMD_RV_RESOURCE_IMPORT`
(0x10002, gpuserver's own, said in its ready file's "features") names the backing file (dev, inode);
gpuserver finds the resource, attaches it to the importer's context (ring records `RES_ATTACH` 12,
`RES_DETACH` 13: `virgl_renderer_ctx_attach_resource` in vrend, `gpu/vrend/rvgpu.c`) and answers with what
it is; a resource lives while any connection holds it. This is also what dmabufs between processes needed.

The page draws a frame into the two textures in the executor's Worker (`tools/www/gpu-exec.js`,
`vdecFrame`): `texImage2D(VideoFrame)` (the browser's YUV to RGB) into a texture of ours, then two draws back
to Y and CbCr with the inverse of the frame's colour-space matrix (texelFetch, 2×2 mean for chroma), vrend's
GL state saved and restored around it, the ring run up to date first (the player's draws from the surface's
last picture were sent before). Then the slot's "ready" word, which `vaSyncSurface` waits for. (The WebGPU executor, `?exec=webgpu`, does the same
in its own Worker: see "On the WebGPU executor" below.) The round trip
through 8-bit RGB is not exact (about one code value; chroma filtered by the browser's upsampling;
out-of-gamut colours clip; full range is asked for so luma does not clip): WebGL cannot give a VideoFrame's
planes as textures. `?vdec=exact` uses copyTo + texSubImage2D (exact bytes; a readback and an upload).

### On the WebGPU executor (VK-14, `tools/www/rvgpu-worker.js`, "video frames")

The same message port (`vdec-port` -> `vdec-frame`) goes to `rvgpu-worker.js` when the executor is WebGPU
(`rvGpu.vdecPort`, `tools/www/gpu-exec.js`; a WebGPU executor that waits for the first GPU program (VK-15) keeps the
port until its Worker starts, and a Worker that finds no WebGPU device hands the port back for vrend's). There is no GL
here, so what vrend does with `texImage2D` is done in WGSL:
- the surface's two textures are the ones `rvgpu_present_grab(res)` gives for the two resources (vfront's zink image of
  each virgl resource, `R8_UNORM` and `RG8_UNORM`; vfront's resources are global, so the guest's import in the player's
  own open needs no attach on this executor);
- the frame is imported as a `GPUExternalTexture` (`device.importExternalTexture`, no copy), and two render passes read it
  with `textureLoad` at their own pixel: luma into the R8 texture, the mean of each 2x2 block's Cb/Cr into the R8G8 one,
  through the inverse of the frame's colour-space matrix (the same `vdecMatrix` as gpu-exec.js; row 0 of the frame is
  row 0 of the texture, as in the vrend path). If the browser will not import the frame, `copyExternalImageToTexture` into
  an RGBA8 texture and the same passes (stats `vdecRgba`); if the shader path fails altogether, or `?vdec=exact`, or the
  frame's visible rect is not its display size, `VideoFrame.copyTo` of the planes and `queue.writeTexture` (`vdecExact`:
  bit for bit, at a readback and an upload);
- frames are drawn between two ring drains (the Worker's loop drains, then pumps the queue), so the guest's draws that
  came before the frame are recorded first, and what the guest sends after the "ready" word runs after the draw on the
  one queue. The ready word is written whatever happened;
- counts: `rvGpu.stats.exec.vdecFrames` (received), `vdecDrawn`, `vdecExternal`/`vdecRgba`/`vdecExact` (by path),
  `vdecSkipped` (given back undrawn: no textures for the resources, or another format), `vdecErrors`, `vdecMs`,
  `vdecLastMs`, `vdecLast`, `vdecPath`, `vdecError`.

Under Node (`tools/gpu-nm.sh mpv`, `gpu/tools/vdec-draw-test.js`) the external-texture import cannot run (dawn.node takes no
VideoFrame), so those runs exercise the RGBA path and the exact path; the import itself is the owner's check
(`the manual test sheets`, VK-14).

### Copy (vaGetImage)

`vaDeriveImage` is refused, so FFmpeg copies with `vaCreateImage` + `vaGetImage`. An image's buffer is a
memory file from gpuserver; vaGetImage asks the page to `copyTo` the surface's frame into it (NV12 layout of
the image; I420 frames from software decoders are interleaved). The page keeps each surface's newest
VideoFrame (closing the oldest past `?vdecframes=` MiB, 384 by default). `vaPutImage` is accepted and not kept
(mpv uploads a picture to probe export).

## Decision 3: codecs and what the browser says

The Worker asks `VideoDecoder.isConfigSupported` per codec at 4096×2304, 3840×2160 and 1920×1080,
`prefer-hardware` first, then `no-preference`, and writes the answers into the ring's header; the driver
offers only those VA profiles, up to the largest size said yes to. Streams are configured with the codec
string of the headers the driver made (`avc1.6400LL`, `hev1.1.6.LNNN.B0`, `vp09.00.LL.08`, `av01.0.LLM.08`),
`optimizeForLatency`, and colorSpace BT.709 full range (what the SPS/VUI say). A decoder error closes it
(WebCodecs); the pictures in it fail (`VA_STATUS_ERROR_DECODING_ERROR`), and a new decoder waits for a key
chunk, as after a seek into a GOP.

| VA profile | codec string | built |
| --- | --- | --- |
| H264ConstrainedBaseline, Main, High | avc1.6400LL | yes |
| HEVCMain | hev1.1.6.LNNN.B0 | yes |
| VP8Version0_3 | vp8 | yes (Chrome: software, still native) |
| VP9Profile0 | vp09.00.LL.08 | yes |
| AV1Profile0 | av01.0.LLM.08 | yes |
| 10-bit (H264High10, HEVCMain10, VP9Profile2, AV1 10-bit) | | no |

## Decision 4: waiting and timing

VA has no timestamps; the player keeps its own. A chunk's timestamp is a serial naming its surface. What
the page adds is latency between `vaEndPicture` and the surface being ready; the driver waits by reading its
status file (polling, 0.1 to 2 ms apart; no round trip, no lock held, so a player's decoding thread goes on
while its display thread waits). It times out after `RVVDEC_TIMEOUT` ms (10000) or when the page's beat
stops for 3 s. Room in the chunk area (an 8 MiB ring in a memory file, bigger for bigger chunks) is waited
for the same way. Audio is untouched.

## Protocols

### Socket (driver -> gpuserver), vtest framing `{length in dwords, command}` + dwords

The driver says `CREATE_RENDERER` with its program's name first, then:

| cmd | id | args | answer |
| --- | --- | --- | --- |
| VDEC_HELLO | 0x10010 | version 1 | status (0 ok, 1 no page decoder yet: asked again for RVVDEC_WAIT s, 2 page gone, 4 no WebCodecs), n, n × {codec, profile, max_w, max_h, flags (1 hw, 2 sw, 4 on caps[0]: zero copy)}; then the status file (SCM_RIGHTS) |
| VDEC_ALLOC | 0x10011 | size | mem, status; then the file |
| VDEC_FREE | 0x10012 | mem | — |
| VDEC_STREAM | 0x10013 | stream, codec, profile, level, width, height, flags | — |
| VDEC_STREAM_END | 0x10014 | stream | — |
| VDEC_SURFACE | 0x10015 | slot, width, height, res_y, res_uv | — |
| VDEC_DECODE | 0x10016 | stream, slot, seq, mem, offset, size, flags (1 key, 2 hidden, 4 draw), end (bytes written so far) | — |
| VDEC_COPY | 0x10017 | slot, id, seq, mem, off_y, pitch_y, off_uv, pitch_uv, width, height, size | — |
| VDEC_EXPORT | 0x10018 | slot | — |
| VDEC_SLOT_END | 0x10019 | slot | — |

Codecs: 1 H.264, 2 HEVC, 3 VP8, 4 VP9, 5 AV1. Slots are VA surface ids (1..4095).

### The status file (per client; the page writes, the driver reads)

0 magic "RVVS", 4 the page's beat, 8 state (gpuserver: 1 open, 2 the page is gone), 12 chunk bytes taken;
from 4096 per slot 32 bytes `{decoded, ready (drawn), copied (copy id), copy_status, error}` (serials).

### The vdec ring (gpuserver -> page)

At the announcement's `vdec` (the `rv-gpu` BroadcastChannel; the ready file says `"vdec"` too): a 4 KiB
header (0 magic "RVVD", 4 version 1, 8 pid, 12 heartbeat, 16 data size, 20 data offset, 24 head, 28 doorbell;
page: 64 tail, 68 state (1 ready, 3 no WebCodecs), 72 beat, 76 caps n, 80 flags (1 zero copy possible),
1024 caps), then 256 KiB of records as in the GPU ring (`{op, client, serial, bytes}` + payload, 16-byte
aligned, PAD). Ops: 1 OPEN [status address], 2 CLOSE, 3 MEM [mem, address, size], 4 MEM_END [mem],
5 STREAM, 6 STREAM_END, 7 SURFACE, 8 DECODE [stream, slot, seq, address, size, flags, end], 9 COPY [slot, id,
seq, address, off_y, pitch_y, off_uv, pitch_uv, width, height, size], 10 EXPORT, 11 SLOT_END.

## Who does what

| piece | file |
| --- | --- |
| VA driver | gpu/vdec/driver/ (driver.c: VA's interface; conn.c: the socket and the status file; h264.c, hevc.c, vp8.c, vp9.c, av1.c; build.sh) |
| gpuserver: video clients, the vdec ring; resource import | gpu/server/vdec.js; gpu/server/server.js |
| rv: PRIME import across opens | rv/virtgpu.c |
| vrend: attach/detach | gpu/vrend/rvgpu.c (built by gpu/vdec/build-vrend.sh against the VM's virglrenderer build), tools/www/gpu-exec.js |
| page | tools/www/vdec-worker.js, tools/www/vdec.js, tools/www/gpu-exec.js (vdecFrame, vdecPort), tools/www/rvgpu-worker.js (video frames: the WebGPU executor) |
| guest | tools/x/desktop (with `RVGPU_DRM=1`: `LIBVA_DRIVER_NAME=rvwasm LIBVA_DRIVERS_PATH=/run/rvgpu/va GST_VA_ALL_DRIVERS=1`; `RVVDEC=0` not), tools/stage.sh (the driver into /usr/lib/rvgpu/va), gpu/vdec/guest/run.sh |
| tests | gpu/vdec/test/: native.sh, vdec-native-server.py, vdec-ref.c, export-test.c, js-test.js, check-dump.sh, chunks.py, make-media.sh; gpu/tools/vdec-stub.js (node-machine; `VDEC_STUB_GPU=executor`: a test card to the real WebGPU executor); gpu/tools/vdec-draw-test.js (the WebGPU executor's drawing on a Dawn device), `tools/gpu-nm.sh mpv` (mpv --hwdec=vaapi on the real executor) |

Driver environment: `RVVDEC_DEBUG=1`, `RVVDEC_DUMP=FILE` (every chunk), `RVVDEC_TIMEOUT=MS`, `RVVDEC_WAIT=S`,
`RVVDEC_CODECS=h264,vp9` (offer only those), `RVVDEC_NO_GPU=1` (no export), `RVVDEC_STATS=0`.

## Verified natively (build VM)

- **Bitstreams** (`gpu/vdec/test/native.sh`: Debian's riscv64 ffmpeg under native rv with `-hwaccel vaapi`,
  our driver, `vdec-native-server.py` standing in for gpuserver and the page with libavcodec as the stateful
  decoder, frames copied back through vaGetImage, compared with a software decode of the same file, frame
  by frame): every frame the same for H.264 High with B-frames, CQM + weighted prediction, 4 slices / 5
  refs, CAVLC Main, Baseline, intra-only, 1080p; HEVC Main with B-frames, and with WPP + 2 slices + scaling
  lists + weighted bi-prediction, 1080p; VP9 with alt-refs (hidden frames), 4K; VP8 with alt-refs, 720p; AV1
  from SVT-AV1 (mini-GOP 16, 21-29 hidden frames), libaom (alt-refs), 4K. H.264 4:2:2 falls back to software
  (profile not offered); interlaced H.264 is refused per picture. (FFmpeg's H.264 decoder drops pictures that
  come out of order when told there is no reordering, unlike Chrome's: the stand-in is told the stream's real
  reordering, `vdec-ref.c` says why.)
- **Export and import** (`export-test.c` under native rv): two opens of the render node, the driver exports a
  surface, the other open imports both planes and finds the same resources, imports again to the same handle.
- **The JavaScript halves** (`js-test.js`: vdec.js and the Worker's core in Node with a stand-in decoder):
  records, status words, NV12 and I420 copies, draws, decoder errors and the key chunk rule.

## Verified under node-machine (the machine's kernel and rv in wasm; Node; stand-ins for WebCodecs and the GPU)

`build/nm-run.sh` (this tree's rv; the bundle in the initramfs) with `gpu/tools/gpu-stub.js` and
`gpu/tools/vdec-stub.js` (the real vdec-worker.js core with a stand-in VideoDecoder):
- `vainfo`: libva on the Wayland display (the window server's dmabuf feedback names the render node) loads
  the driver, which gets the page's caps through gpuserver.
- `ffmpeg -hwaccel vaapi` (copy): 60 chunks, 60 copies, no errors; the chunks the page got, decoded on their
  own (`check-dump.sh`), are the original's pictures exactly.
- mpv `--hwdec=vaapi`: see the end of this file (results).

## Verified on the WebGPU executor under Node (VK-14, 2026-10-13; Dawn's Node bindings on the Mac's Metal, no browser)

- `gpu/tools/vdec-draw-test.js` (the Worker's own drawing code, a real Dawn device, test-card frames): 19 checks. The shader path
  (an RGBA copy standing in for the import) puts luma within 1 code and chroma within 1 code of the card's own planes (full range
  BT.709; BT.709 limited and BT.601 round-trip to 0 codes where the RGB was not clipped), the exact path is bit for bit, a
  picture smaller than its surface leaves the rest untouched, missing or wrong-format textures still write the ready word, 20 frames
  in a row all arrive in order, no validation errors.
- `tools/gpu-nm.sh mpv` (mpv --hwdec=vaapi in the machine, the real executor, the stand-in decoder sending a test card): 60 chunks,
  57 frames drawn of 57 sent (shader path from the RGBA copy, 7.4 ms a frame; `--exact`: 54 of 54, 0.7 ms), 0 skipped, 0 errors,
  and 24 of 24 window PNGs show the card in mpv's window (eight bars in order, ramp rising to the right, bars on top). This needs
  an rv with PRIME import across opens (the dev server's older initramfs says "PRIME import ... not done yet" and mpv's EGL import
  then fails): the script overlays `build/vdec-nm/rv` when it is there.
- Not run under Node: `device.importExternalTexture` (the browser's) and the real decoder.

## Not verified (the owner's checks): anything in a browser

Chrome's VideoToolbox decode of these chunks, isConfigSupported's answers on the M4 Max, the executor's draw
into vrend's textures (WebGL with VideoFrame), mpv's picture on screen, timing.

## Risks

- Chrome's decoders are stricter than FFmpeg's (level, VUI, HEVC sub-layers); only the owner's checks tell.
- Seeking into a GOP: delta chunks before the next key chunk fail (WebCodecs wants a key chunk); players
  recover at the next key frame.
- Zero-copy colour is approximate (above); `?vdec=exact` is exact.
- A decoder holding frames back (reordering despite our VUI) would make vaSyncSurface time out: Chrome's
  sources say it does not.
- Surface reuse ordering: the player's GL commands that still sample a surface travel on the GPU socket, a
  new decode into it on the video socket; the executor runs its ring up to date before each draw, which
  closes it unless both arrive within one turn.
- On `?exec=webgpu` the frame's colour goes through the browser's conversion to sRGB on its way into the shader
  (`importExternalTexture` converts to a destination colour space; BT.709's transfer function differs a little from sRGB's,
  and the shader inverts only the matrix, as the vrend path does with WebGL's own YUV to RGB): expect tone differences in the
  darks between `?exec=webgpu` and `?exec=vrend` and between either and `?vdec=exact`. The owner's sheet compares them.
- A WebGPU Worker that dies before it says "ready" (a script error) has the decoder's port inside it: vrend takes the ring but
  the port is lost, and zero-copy frames are not drawn (the driver's wait ends at its 10 s timeout). The no-device fallback
  hands the port back. Device loss (VK-13) likewise terminates the Worker: programs that are running keep waiting.

## Not done

10-bit (VideoToolbox gives P010, which WebCodecs cannot copy as such), interlaced H.264, POC type 1, HEVC range
extensions, AV1's corner cases above, NV12 dmabufs in the window server (`vo=dmabuf-wayland`, which would let
the page draw frames exactly), a V4L2 front end, Firefox's VA-API path (untested).
