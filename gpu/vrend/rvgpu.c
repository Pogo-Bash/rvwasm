/*
 * The page's GPU executor's C half: virglrenderer (vrend, its GL ES path) on the WebGL2 context the
 * JavaScript half (tools/www/gpu-exec.js) made current, and the few entry points that half calls for the
 * records of gpuserver's ring (gpu/PROTOCOL.md in branch gpu-guest).
 *
 * This module has its own memory (Emscripten's heap); the machine's memory is another WebAssembly memory.
 * So command buffers and transfer data are copied into the heap by the JavaScript half before a call
 * (rvg_buffer() gives the place), and read data copied out after one.
 *
 * One GL context for everything: WebGL contexts share no objects, so every context vrend asks for is the
 * one context, and making one current does nothing.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <emscripten.h>
#include <emscripten/console.h>

#include "virglrenderer.h"
#include "virgl_hw.h"

static uint32_t last_fence;

static void write_fence(void *cookie, uint32_t fence)
{
	(void)cookie;
	last_fence = fence;
}

static virgl_renderer_gl_context create_gl_context(void *cookie, int scanout, struct virgl_renderer_gl_ctx_param *param)
{
	(void)cookie;
	(void)scanout;
	/* WebGL2 is GL ES 3.0: say no to anything newer, so vrend settles for what there is. */
	if (param->major_ver > 3 || (param->major_ver == 3 && param->minor_ver > 0))
		return NULL;
	return (virgl_renderer_gl_context)1;
}

static void destroy_gl_context(void *cookie, virgl_renderer_gl_context ctx)
{
	(void)cookie;
	(void)ctx;
}

static int make_current(void *cookie, int scanout, virgl_renderer_gl_context ctx)
{
	(void)cookie;
	(void)scanout;
	(void)ctx;
	return 0;
}

static struct virgl_renderer_callbacks callbacks = {
	.version = 1,
	.write_fence = write_fence,
	.create_gl_context = create_gl_context,
	.destroy_gl_context = destroy_gl_context,
	.make_current = make_current,
};

/* A place in the heap for data going in or out, grown as needed. */
static uint8_t *buffer;
static size_t buffer_size;

EMSCRIPTEN_KEEPALIVE uint8_t *rvg_buffer(uint32_t size)
{
	if (size > buffer_size) {
		free(buffer);
		buffer_size = (size + 0xffff) & ~(size_t)0xffff;
		buffer = aligned_alloc(16, buffer_size);
		if (!buffer)
			buffer_size = 0;
	}
	return buffer;
}

/*
 * vrend's messages straight to the page (gpu-exec.js passes them on), not through stdio: its own logger's
 * fputs(stderr) trapped with "null function" in __fwritex on a failed shader compile's long message, and a
 * trap leaves the module unusable. Long messages (a shader's source with its errors) are cut.
 */
static void logger(enum virgl_log_level_flags level, const char *message, void *user_data)
{
	(void)level;
	(void)user_data;
	char line[2048];
	snprintf(line, sizeof(line), "%s", message);
	size_t n = strlen(line);
	while (n && line[n - 1] == '\n')
		line[--n] = 0;
	emscripten_err(line);
}

EMSCRIPTEN_KEEPALIVE int rvg_init(void)
{
	/* (the cookie only has to be there: virgl_renderer_init refuses a NULL one) */
	static int cookie;
	virgl_set_log_callback(logger, NULL, NULL);
	return virgl_renderer_init(&cookie, VIRGL_RENDERER_USE_GLES, &callbacks);
}

/* The caps (set 2) into the buffer; returns their size in bytes. */
EMSCRIPTEN_KEEPALIVE uint32_t rvg_caps(void)
{
	uint32_t max_ver = 0, max_size = 0;
	virgl_renderer_get_cap_set(2, &max_ver, &max_size);
	if (!max_size)
		return 0;
	uint8_t *b = rvg_buffer(max_size);
	memset(b, 0, max_size);
	virgl_renderer_fill_caps(2, max_ver, b);
	/*
	 * No transfers inside command buffers (VIRGL_CCMD_TRANSFER3D, and COPY_TRANSFER3D from staging buffers,
	 * which Mesa only uses with them): vrend would read and write those through the resource's backing
	 * as an iovec, and the backing is in the machine's memory, not in this module's. Without the bit
	 * Mesa's virgl sends every upload and readback as a vtest TRANSFER_PUT2/GET2, which reach us as ring
	 * records (TRANSFER_PUT/GET) that gpu-exec.js copies across.
	 */
	((union virgl_caps *)b)->v2.capability_bits &= ~(VIRGL_CAP_TRANSFER | VIRGL_CAP_COPY_TRANSFER);
	/*
	 * And copy transfers from the host with them: Mesa reads textures back through its staging buffer when this
	 * bit is there, and it makes that buffer only with COPY_TRANSFER: without it, a NULL dereference in
	 * virgl_staging_alloc the first time a program writes part of a texture (mpv's frames). rv's render node
	 * takes the bit away too (rv/virtgpu.c caps_fix), for executors that still give it.
	 */
	((union virgl_caps *)b)->v2.capability_bits_v2 &= ~VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS;
	return max_size;
}

/* The name (UTF-8) is in the buffer. */
EMSCRIPTEN_KEEPALIVE int rvg_ctx_create(uint32_t ctx, uint32_t name_len)
{
	return virgl_renderer_context_create(ctx, name_len, (const char *)buffer);
}

EMSCRIPTEN_KEEPALIVE void rvg_ctx_destroy(uint32_t ctx)
{
	virgl_renderer_context_destroy(ctx);
}

/* A resource without backing (transfers bring their data along), attached to its context. */
EMSCRIPTEN_KEEPALIVE int rvg_res_create(uint32_t ctx, uint32_t res, uint32_t target, uint32_t format, uint32_t bind,
					uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size,
					uint32_t last_level, uint32_t nr_samples, uint32_t flags)
{
	struct virgl_renderer_resource_create_args a = {
		.handle = res, .target = target, .format = format, .bind = bind, .width = width, .height = height,
		.depth = depth, .array_size = array_size, .last_level = last_level, .nr_samples = nr_samples,
		.flags = flags,
	};
	int r = virgl_renderer_resource_create(&a, NULL, 0);
	if (r == 0)
		virgl_renderer_ctx_attach_resource(ctx, res);
	return r;
}

EMSCRIPTEN_KEEPALIVE void rvg_res_unref(uint32_t ctx, uint32_t res)
{
	virgl_renderer_ctx_detach_resource(ctx, res);
	virgl_renderer_resource_unref(res);
}

/*
 * A resource another context made, imported by this one (gpuserver's RES_ATTACH: a dmabuf passed between
 * processes, or a video surface libva made, sampled by a player's GL), and let go of again (RES_DETACH) while
 * others still hold it. vrend looks resources up per context.
 */
EMSCRIPTEN_KEEPALIVE int rvg_res_attach(uint32_t ctx, uint32_t res)
{
	virgl_renderer_ctx_attach_resource(ctx, res);
	return 0;
}

EMSCRIPTEN_KEEPALIVE void rvg_res_detach(uint32_t ctx, uint32_t res)
{
	virgl_renderer_ctx_detach_resource(ctx, res);
}

/* ndw command words are in the buffer. */
EMSCRIPTEN_KEEPALIVE int rvg_submit(uint32_t ctx, uint32_t ndw)
{
	return virgl_renderer_submit_cmd(buffer, ctx, ndw);
}

/* `len` bytes in the buffer are the box's data (as they were in the backing from `offset` on). Stride 0:
 * vrend's own (the level's width), as vtest's transfers have it. */
EMSCRIPTEN_KEEPALIVE int rvg_put(uint32_t ctx, uint32_t res, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
				 uint32_t w, uint32_t h, uint32_t d, uint32_t len)
{
	struct virgl_box box = { x, y, z, w, h, d };
	struct iovec iov = { buffer, len };
	return virgl_renderer_transfer_write_iov(res, ctx, level, 0, 0, &box, 0, &iov, 1);
}

/* The box's data into the buffer (`len` bytes), to be copied into the backing at `offset`. */
EMSCRIPTEN_KEEPALIVE int rvg_get(uint32_t ctx, uint32_t res, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
				 uint32_t w, uint32_t h, uint32_t d, uint32_t len)
{
	struct virgl_box box = { x, y, z, w, h, d };
	struct iovec iov = { rvg_buffer(len), len };
	return virgl_renderer_transfer_read_iov(res, ctx, level, 0, 0, &box, 0, &iov, 1);
}

/* The GL texture of a resource (Emscripten's name for it: GL.textures[n] in JavaScript), its size and
 * flags; 0 when it has none. */
static struct virgl_renderer_resource_info info;

EMSCRIPTEN_KEEPALIVE uint32_t rvg_res_texture(uint32_t res)
{
	memset(&info, 0, sizeof(info));
	if (virgl_renderer_resource_get_info(res, &info))
		return 0;
	return info.tex_id;
}

EMSCRIPTEN_KEEPALIVE struct virgl_renderer_resource_info *rvg_res_info(void)
{
	return &info;
}

/* Fences: vrend's ctx-0 fences, polled (glClientWaitSync with timeout 0). */
EMSCRIPTEN_KEEPALIVE int rvg_fence(uint32_t id, uint32_t ctx)
{
	return virgl_renderer_create_fence(id, ctx);
}

EMSCRIPTEN_KEEPALIVE uint32_t rvg_poll(void)
{
	virgl_renderer_poll();
	return last_fence;
}
