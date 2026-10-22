/*
 * rvgpu: the page host of the GPU on WebGPU. The machine's memory (a separate,
 * shared WebAssembly.Memory) and gpuserver's ring (gpu/PROTOCOL.md, ring v2)
 * as seen from C. See gpu/webgpu/INTERFACES.md, section 3.
 *
 * "Guest address": an offset in the machine's memory. C code cannot point into
 * that memory in the page; everything goes through these functions. Natively
 * (tests) a host block stands in for it (rvgpu_guest_set_host).
 */
#ifndef RVGPU_H
#define RVGPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A record in the ring's data area: 16 bytes, then `bytes` of payload. */
struct rvgpu_record {
	uint32_t op, ctx, serial, bytes;
};

/* Ring ops (PROTOCOL.md v2; 9.. are ring v3, venus: INTERFACES.md 7.1). */
enum {
	RVGPU_OP_PAD = 0, RVGPU_OP_CTX_CREATE = 1, RVGPU_OP_CTX_DESTROY = 2, RVGPU_OP_RES_CREATE = 3,
	RVGPU_OP_RES_UNREF = 4, RVGPU_OP_SUBMIT = 5, RVGPU_OP_TRANSFER_PUT = 6, RVGPU_OP_TRANSFER_GET = 7,
	RVGPU_OP_BLOB_CREATE = 8, RVGPU_OP_VN_CTX_INIT = 9, RVGPU_OP_VN_SUBMIT = 10, RVGPU_OP_VN_BLOB_CREATE = 11,
	RVGPU_OP_COUNT = 12,
};

/* Front ends (INTERFACES.md 7.1). A context's records go to the front end that owns it: every context is
 * virgl's (vfront) until a VN_CTX_INIT record makes it venus' (vkr), until its CTX_DESTROY. */
enum { RVGPU_FRONT_VIRGL = 0, RVGPU_FRONT_VENUS = 1, RVGPU_FRONT_COUNT };

/* Ring header word offsets (bytes from `ring`). */
enum {
	RVGPU_RING_MAGIC = 0, RVGPU_RING_VERSION = 4, RVGPU_RING_PID = 8, RVGPU_RING_HEARTBEAT = 12,
	RVGPU_RING_DATA_SIZE = 16, RVGPU_RING_DATA_OFF = 20, RVGPU_RING_HEAD = 24, RVGPU_RING_DOORBELL = 28,
	RVGPU_RING_SUBMITTED = 32, RVGPU_RING_EXPORTS_LO = 40, RVGPU_RING_EXPORTS_HI = 44,
	RVGPU_RING_EXPORTS_CAP = 48, RVGPU_RING_TAIL = 64, RVGPU_RING_DONE = 68, RVGPU_RING_EXEC_HEARTBEAT = 72,
	RVGPU_RING_EXEC_STATE = 76, RVGPU_RING_EXEC_ERROR = 80, RVGPU_RING_CAPS_LEN = 84,
	RVGPU_RING_VCAPS_LEN = 88, RVGPU_RING_CTX_DONE = 1024,
};
#define RVGPU_RING_MAGIC_VALUE 0x50475652u	/* "RVGP" */

int rvgpu_init(void);
int rvgpu_ring_attach(uint64_t ring, uint32_t ring_size, uint64_t caps, uint32_t caps_size);
int rvgpu_ring_drain(void);
typedef void (*rvgpu_op_fn)(void *front, const struct rvgpu_record *rec, const void *payload);
/* rvgpu_ring_register(op, ...) is rvgpu_ring_register_front(RVGPU_FRONT_VIRGL, op, ...) */
void rvgpu_ring_register(uint32_t op, rvgpu_op_fn fn, void *front);
void rvgpu_ring_register_front(unsigned front_kind, uint32_t op, rvgpu_op_fn fn, void *front);
/* Called at the end of every rvgpu_ring_drain (vkr: its rings and fences, which have no thread). It returns
 * true when it wants the next drain soon: rvgpu_poll_ms() is then 1 instead of 500, the Worker's wait for
 * the doorbell. */
void rvgpu_ring_set_poll(bool (*poll)(void *data), void *data);
int rvgpu_poll_ms(void);
/* What the executor is doing (a short text front ends and wgvk may write, for the Worker to say when a drain
 * takes long: rvgpu-worker.js reads rvgpu_debug_where()). */
extern char rvgpu_where[128];
const char *rvgpu_debug_where(void);
/* Words a front end may keep current (vkr: the venus command being run), read by the page from the module's
 * memory when the executor stops moving (rvgpu_debug_words_at: their address). */
extern uint32_t rvgpu_debug_words[8];
uint32_t *rvgpu_debug_words_at(void);
void rvgpu_complete(uint32_t ctx, uint32_t serial);
/* exec_state: 1 ready (caps written), 2 context lost, 3 failed */
void rvgpu_set_state(uint32_t state, uint32_t error);
/* Write caps (virgl caps v2) and set caps_len. */
void rvgpu_set_caps(const void *caps, uint32_t len);
/* Write the venus capset at caps + 2048 and set vcaps_len (INTERFACES.md 7.2); exec_state becomes 1 if no
 * front end has said anything yet (a page with vkr and no vfront still serves venus). */
void rvgpu_set_venus_caps(const void *capset, uint32_t len);

uint32_t rvgpu_guest_load32(uint64_t addr);
void rvgpu_guest_store32(uint64_t addr, uint32_t v);
void rvgpu_guest_read(void *dst, uint64_t addr, size_t len);
void rvgpu_guest_write(uint64_t addr, const void *src, size_t len);
void rvgpu_guest_upload_buffer(WGPUQueue q, WGPUBuffer b, uint64_t boff, uint64_t addr, size_t len);
void rvgpu_guest_upload_texture(WGPUQueue q, const WGPUTexelCopyTextureInfo *dst, uint64_t addr,
				const WGPUTexelCopyBufferLayout *layout, const WGPUExtent3D *size);
void rvgpu_guest_from_mapped(WGPUBuffer mapped, uint64_t boff, uint64_t addr, size_t len);
void *rvgpu_guest_ptr(uint64_t addr, size_t len);
void rvgpu_guest_set_host(void *base, uint64_t size);

/* Presenting (T7; INTERFACES.md 9). A window whose buffer is GPU resource `res` (wlserver's slot format 2)
 * shows a texture: the page's Worker asks for it (rvgpu_present_grab, rvgpu_page.c) once the ring's `done`
 * has passed the frame, and draws it into its canvas.
 * RVGPU_PRESENT_Y_0_TOP: row 0 of the texture is the top of the picture (Vulkan's way, and virgl resources
 * made with VIRGL_RESOURCE_Y_0_TOP); without it row 0 is the bottom (GL's way: virgl's window buffers).
 * The texture's own format says how to read its texels (BGRA formats need nothing from the reader), so
 * RVGPU_PRESENT_BGRA is only for producers whose texture holds BGRA bytes in an RGBA format. */
enum { RVGPU_PRESENT_Y_0_TOP = 1, RVGPU_PRESENT_BGRA = 2, RVGPU_PRESENT_BUSY = 0x80000000u };
struct rvgpu_present_image {
	WGPUTexture tex;		/* not referenced: valid until the producer next changes the resource */
	uint32_t width, height;		/* the picture, from the texture's origin (level 0, layer 0) */
	uint32_t flags;			/* RVGPU_PRESENT_* */
};
/* Textures named outright (a producer that knows `res` at creation) ... */
void rvgpu_present_register(uint32_t res, WGPUTexture tex, uint32_t flags);
void rvgpu_present_unregister(uint32_t res);
/* ... or found when asked: a front end adds a resolver that returns true and fills `out` when `res` is one of
 * its resources (vfront: a virgl resource's zink image; vkr: a blob whose memory a WSI image is bound to). */
typedef bool (*rvgpu_present_resolve_fn)(void *front, uint32_t res, struct rvgpu_present_image *out);
void rvgpu_present_add_resolver(rvgpu_present_resolve_fn fn, void *front);
/* True while the ring is being drained (a front end is in the middle of a record, perhaps suspended by JSPI
 * waiting for the GPU): resolvers that would have to flush a front end's work must not run then; the page
 * asks again (rvgpu_present_grab says "busy"). */
bool rvgpu_ring_busy(void);
/* The registered texture of `res`, else the first resolver's answer. */
bool rvgpu_present_find(uint32_t res, struct rvgpu_present_image *out);
WGPUTexture rvgpu_present_lookup(uint32_t res, uint32_t *flags);
/* For vkr (T5), page only (rvgpu_page.c): resource `res` is the blob of venus memory `memory` (a VkDeviceMemory
 * of wgvk device `device`, both as 64-bit handles). A window that shows it (a venus WSI image: the guest's WSI
 * hands the window server the memory's dma-buf) shows the image bound to that memory (wgvk_memory_image),
 * top row first. Call at VN_BLOB_CREATE; unregister at RES_UNREF (rvgpu_present_unregister). */
void rvgpu_present_register_memory(uint32_t res, uint64_t device, uint64_t memory);

#ifdef __cplusplus
}
#endif

#endif
