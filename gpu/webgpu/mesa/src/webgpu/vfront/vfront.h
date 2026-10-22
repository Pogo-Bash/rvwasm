/*
 * vfront: the page's virgl decoder. It takes what gpuserver's ring carries (gpu/PROTOCOL.md in branch gpu-guest,
 * ring version 2: contexts, resources with their backing in the machine's memory, SUBMIT command buffers in
 * virgl's wire format, transfers between a resource and its backing) and turns it into Gallium calls on Mesa's
 * zink, which runs on Vulkan: lavapipe natively in the build VM, wgvk (Vulkan on WebGPU) in the page.
 *
 * The guest keeps stock Mesa's virgl driver, so the command stream is exactly what Mesa 25.0.7's virgl_encode.c
 * writes; virglrenderer's vrend_decode.c is the other reference (what a host does with each command). Shaders
 * arrive as TGSI text and go to zink as NIR (vf_shader.c). The caps the guest sees are made from zink's own pipe
 * caps (vf_caps.c): the inverse of what virgl_screen.c does with them.
 *
 * Calls are made from one thread. Nothing here waits for the GPU except vf_ctx_poll() with a timeout: work is
 * flushed with vf_ctx_flush() and its completion (readbacks into backings, query results) is noticed by polling,
 * so in the page the executor never blocks on WebGPU.
 */
#ifndef VFRONT_H
#define VFRONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resource backings live in memory the decoder does not own: natively a mapping of gpuserver-like files, in
 * the page the machine's linear memory, which need not be this module's memory. Addresses are the guest's
 * (the ring's addr_lo/addr_hi). */
struct vf_mem_ops {
	void *cookie;
	/* a pointer through which [addr, addr + size) can be read and written directly, or NULL (then read and
	 * write are used) */
	void *(*ptr)(void *cookie, uint64_t addr, uint64_t size);
	void (*read)(void *cookie, uint64_t addr, void *dst, size_t size);
	void (*write)(void *cookie, uint64_t addr, const void *src, size_t size);
};

struct vf_init_args {
	/* a PFN_vkGetInstanceProcAddr for zink to use instead of the system's Vulkan loader (wgvk's in the page);
	 * NULL: the loader */
	void *vk_get_instance_proc_addr;
	/* how to reach backings; NULL: addresses are plain pointers of this process */
	const struct vf_mem_ops *mem;
	/* VF_DEBUG_* */
	uint32_t debug;
	/* VF_INIT_* */
	uint32_t flags;
};

#define VF_INIT_SINGLE_THREAD (1u << 0) /* no driver threads (the page: every WebGPU call on one thread) */

#define VF_DEBUG_CMD     (1u << 0) /* every command decoded */
#define VF_DEBUG_SHADER  (1u << 1) /* TGSI text and the NIR made from it */
#define VF_DEBUG_ERROR   (1u << 2) /* failed commands (on by default) */
#define VF_DEBUG_SYNC    (1u << 3) /* wait for the GPU after every submit (finding which command hangs) */
#define VF_DEBUG_CAPS    (1u << 4) /* the caps given to the guest */

int vf_init(const struct vf_init_args *args);
void vf_fini(void);

/* The virgl caps (set 2, struct virgl_caps_v2) for GET_CAPS2, written into out; returns their size in bytes
 * (0 when out is too small). */
uint32_t vf_caps(void *out, uint32_t max_size);

/* Replaying a capture made with other caps: the caps the guest was given then (what it assumed of the host,
 * e.g. whether gl_FragCoord's origin needed its own transform). Call after vf_init. */
void vf_set_guest_caps(const void *caps, uint32_t size);

/* The renderer's name, as the guest will show it ("virgl (...)" is added by the guest). */
const char *vf_renderer_name(void);

/* Contexts are numbered by the ring (1..255); 0 is not a valid context. */
int vf_ctx_create(uint32_t ctx, const char *name);
void vf_ctx_destroy(uint32_t ctx);

struct vf_res_create_args {
	uint32_t res;		/* the resource id (global over contexts) */
	uint32_t target, format, bind;	/* virgl/pipe target, virgl format, VIRGL_BIND_* */
	uint32_t width, height, depth, array_size, last_level, nr_samples, flags;
};

/* A resource and its backing ([backing, backing + backing_size), 0 size = none), attached to ctx. */
int vf_res_create(uint32_t ctx, const struct vf_res_create_args *args, uint64_t backing, uint64_t backing_size);
void vf_res_unref(uint32_t ctx, uint32_t res);

/* The command buffer of a SUBMIT: ndw dwords. Returns 0, or an errno for the first command that failed (the
 * rest of the buffer is skipped, as vrend does). */
int vf_submit(uint32_t ctx, const uint32_t *dw, uint32_t ndw);

struct vf_box {
	uint32_t x, y, z, w, h, d;
};

/* TRANSFER_PUT: the box's data is in the backing from `offset` on, with the natural strides of the level (as
 * vtest's TRANSFER_PUT2 has it). */
int vf_transfer_put(uint32_t ctx, uint32_t res, uint32_t level, const struct vf_box *box, uint64_t offset);

/* TRANSFER_GET: the box's data into the backing from `offset` on (natural strides). The bytes are there once
 * the serial of the flush that follows has completed (vf_ctx_poll). */
int vf_transfer_get(uint32_t ctx, uint32_t res, uint32_t level, const struct vf_box *box, uint64_t offset);

/* Everything given to ctx since the last flush is one unit of completion called `serial` (the ring's serial of
 * the last record in it). Submits the GPU work. Returns 1 when completing it will write backings (readbacks,
 * query results: wait for it with vf_ctx_poll), 0 when it will not, or a negative errno. */
int vf_ctx_flush(uint32_t ctx, uint32_t serial);

/* The newest serial of ctx such that it and all before it are complete; finishes what completed work leaves
 * to do (readbacks into backings, query results). timeout_ns 0 only looks; UINT64_MAX waits for everything
 * flushed so far. */
uint32_t vf_ctx_poll(uint32_t ctx, uint64_t timeout_ns);

/* For presenting (T7): the pipe_resource of a resource id, or NULL. */
struct pipe_resource;
struct pipe_resource *vf_res_pipe(uint32_t res);
/* Flush work that is with the driver but not submitted (TRANSFER_PUTs complete without a flush; the next
 * flush takes them along): before the page shows a resource. Only between records. */
void vf_flush_pending(void);

/* Counters since vf_init. */
struct vf_stats {
	uint64_t submits, commands, draws, failed;
	uint64_t shaders, shader_failed, shader_ns;
	uint64_t puts, put_bytes, gets, get_bytes;
	uint64_t flushes;
};
void vf_get_stats(struct vf_stats *stats);

#ifdef __cplusplus
}
#endif

#endif
