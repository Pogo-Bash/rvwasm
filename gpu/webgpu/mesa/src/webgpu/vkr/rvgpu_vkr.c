/*
 * rvgpu_vkr: vkr, virglrenderer's venus renderer, as rvgpu's venus front end (T5; INTERFACES.md 7).
 *
 * gpuserver speaks vtest to the guest's venus driver (Mesa's libvulkan_virtio with VN_DEBUG=vtest) and puts
 * what needs the renderer into the ring as v3 records, which rvgpu.c hands to the context's front end:
 *
 *   VN_CTX_INIT    {capset_id, name (NUL-terminated)}: CONTEXT_INIT; the context is venus' from now on
 *   VN_BLOB_CREATE {res, blob_type, blob_flags, size_lo, size_hi, blob_id_lo, blob_id_hi, addr_lo, addr_hi}:
 *                  RESOURCE_CREATE_BLOB; gpuserver made the guest pages (a file the guest maps) at addr.
 *                  blob_id 0: shm (rings, reply and command streams); else the VkDeviceMemory of that
 *                  object id, whose bytes are those pages from now on (wgvk's GUEST window)
 *   VN_SUBMIT      SUBMIT_CMD2: {n_batches, then per batch {cmd_off, cmd_size, sync_off, sync_count, ring_idx,
 *                  flags} (dword offsets and sizes in this payload), then the command dwords, then per sync
 *                  {addr_lo, addr_hi, value_lo, value_hi}}: run each batch's commands, then when the batch's
 *                  ring (a queue's timeline, 0 = the CPU's) has done what was submitted before, write each
 *                  value into its u64 word (gpuserver's sync, which answers SYNC_READ/SYNC_WAIT from it)
 *   RES_UNREF, CTX_DESTROY  as in ring v2
 *
 * vkr has no threads here (virglrenderer-page.diff): what its ring, ring-monitor and queue threads did runs
 * from rvgpu's poll hook at the end of every drain; while a ring is not idle or a fence is pending, the
 * Worker drains again after 1 ms instead of waiting for the doorbell.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkr_renderer.h"
#include "vkr_guest.h"

#include "rvgpu.h"
#include "rvgpu_vkr.h"

#define CAPSET_VENUS 4
#define VCAPS_OFFSET 2048	/* the venus capset's place in the caps area (INTERFACES.md 7.2) */

/* syncs a batch signals once its fence retires */
struct pending {
	uint32_t ring_idx;
	uint64_t fence_id;
	unsigned n;
	struct { uint64_t addr, value; } *sync;
};

struct vctx {
	bool live, failed;
	uint64_t fence_id;
	struct pending *pend;
	unsigned n_pend, cap_pend;
};

static struct {
	struct vctx ctx[256];
	uint64_t caps;
	unsigned submits, batches;
	bool quiet, stats;
	uint32_t blob_res;	/* the resource VN_BLOB_CREATE is importing (T7) */
	void (*set_guest)(VkDevice dev, VkDeviceMemory mem, uint64_t guest, uint64_t size);
} V;

#define logf(...) do { if (!V.quiet) fprintf(stderr, "rvgpu-vkr: " __VA_ARGS__); } while (0)

static void
on_log(enum virgl_log_level_flags level, const char *message, void *data)
{
	(void)level;
	(void)data;
	fprintf(stderr, "vkr: %s", message);
	size_t n = strlen(message);
	if (!n || message[n - 1] != '\n')
		fputc('\n', stderr);
}

/* a 64-bit sync word: low then high (readers read high, low, high) */
static void
store64(uint64_t addr, uint64_t v)
{
	rvgpu_guest_store32(addr, (uint32_t)v);
	rvgpu_guest_store32(addr + 4, (uint32_t)(v >> 32));
}

/* T7 (INTERFACES.md 9): a blob that is a VkDeviceMemory may be a WSI image's memory, which a window then shows.
 * The front end that shows windows (the page; vkr-vtest's present) provides rvgpu_present_register_memory; a
 * build without one only makes the guest window. */
void rvgpu_present_register_memory(uint32_t res, uint64_t device, uint64_t memory) __attribute__((weak));

static void
guest_hook(VkDevice dev, VkDeviceMemory mem, uint64_t guest, uint64_t size)
{
	if (V.blob_res && guest && rvgpu_present_register_memory) {
		uint64_t d = (uint64_t)(uintptr_t)dev, m;
		memcpy(&m, &mem, sizeof(m));
		rvgpu_present_register_memory(V.blob_res, d, m);
	}
	V.set_guest(dev, mem, guest, size);
}

static void
on_retire(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id)
{
	struct vctx *c = &V.ctx[ctx_id & 255];
	unsigned keep = 0;
	for (unsigned i = 0; i < c->n_pend; i++) {
		struct pending *p = &c->pend[i];
		if (p->ring_idx == ring_idx && p->fence_id <= fence_id) {
			for (unsigned k = 0; k < p->n; k++)
				store64(p->sync[k].addr, p->sync[k].value);
			free(p->sync);
		} else {
			c->pend[keep++] = *p;
		}
	}
	c->n_pend = keep;
}

static const char *
cstr(const void *p, uint32_t bytes, char *buf, size_t size)
{
	size_t n = 0;
	const char *s = p;
	while (n < bytes && n + 1 < size && s[n])
		buf[n] = s[n], n++;
	buf[n] = 0;
	return buf;
}

static void
op_ctx_init(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	const uint32_t *w = payload;
	struct vctx *c = &V.ctx[rec->ctx & 255];
	char name[64];
	cstr(w + 1, rec->bytes >= 4 ? rec->bytes - 4 : 0, name, sizeof(name));
	if (rec->bytes < 4 || w[0] != CAPSET_VENUS) {
		logf("context %u: capset %u is not venus\n", rec->ctx, rec->bytes >= 4 ? w[0] : 0);
		c->failed = true;
		return;
	}
	if (c->live)
		vkr_renderer_destroy_context(rec->ctx);
	*c = (struct vctx){ 0 };
	c->live = vkr_renderer_create_context(rec->ctx, CAPSET_VENUS, strlen(name), name);
	c->failed = !c->live;
	if (!c->live)
		logf("context %u (%s): vkr_renderer_create_context failed\n", rec->ctx, name);
}

static void
op_ctx_destroy(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	(void)payload;
	struct vctx *c = &V.ctx[rec->ctx & 255];
	if (c->live)
		vkr_renderer_destroy_context(rec->ctx);
	/* (fences of a destroyed context never retire; their syncs are gpuserver's to let go of) */
	for (unsigned i = 0; i < c->n_pend; i++)
		free(c->pend[i].sync);
	free(c->pend);
	*c = (struct vctx){ 0 };
}

static void
op_blob_create(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	const uint32_t *w = payload;
	struct vctx *c = &V.ctx[rec->ctx & 255];
	if (!c->live || rec->bytes < 36)
		return;
	const uint32_t res = w[0];
	const uint64_t size = w[3] | (uint64_t)w[4] << 32;
	const uint64_t blob_id = w[5] | (uint64_t)w[6] << 32;
	const uint64_t addr = w[7] | (uint64_t)w[8] << 32;
	V.blob_res = res;	/* (T7: guest_hook below learns which resource the memory is) */
	bool ok = vkr_renderer_import_guest_resource(rec->ctx, res, blob_id, size, addr);
	V.blob_res = 0;
	if (!ok)
		logf("context %u: blob %llu (resource %u, %llu bytes) refused\n", rec->ctx,
		     (unsigned long long)blob_id, res, (unsigned long long)size);
}

static void
op_res_unref(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	const uint32_t *w = payload;
	if (V.ctx[rec->ctx & 255].live && rec->bytes >= 4)
		vkr_renderer_destroy_resource(rec->ctx, w[0]);
	if (rec->bytes >= 4)
		rvgpu_present_unregister(w[0]);	/* (T7) */
}

static void
op_submit(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	const uint32_t *w = payload;
	const uint32_t n_dw = rec->bytes / 4;
	struct vctx *c = &V.ctx[rec->ctx & 255];
	V.submits++;
	if (!c->live || n_dw < 1) {
		rvgpu_complete(rec->ctx, rec->serial);
		return;
	}
	const uint32_t n = w[0];
	if (n > (n_dw - 1) / 6) {
		logf("context %u: a submit of %u batches in %u dwords\n", rec->ctx, n, n_dw);
		rvgpu_complete(rec->ctx, rec->serial);
		return;
	}
	for (uint32_t b = 0; b < n; b++) {
		const uint32_t *h = w + 1 + 6 * b;
		const uint32_t cmd_off = h[0], cmd_size = h[1], sync_off = h[2], sync_count = h[3], ring_idx = h[4];
		if (cmd_off > n_dw || cmd_size > n_dw - cmd_off || sync_off > n_dw ||
		    sync_count > (n_dw - sync_off) / 4) {
			logf("context %u: batch %u out of its record\n", rec->ctx, b);
			break;
		}
		V.batches++;
		/* (vkr decodes in place: give it a copy it may keep pointers into until it returns) */
		if (cmd_size && !vkr_renderer_submit_cmd(rec->ctx, (void *)(w + cmd_off), 4 * cmd_size))
			logf("context %u: batch %u: commands refused\n", rec->ctx, b);
		if (!sync_count)
			continue;
		if (c->n_pend == c->cap_pend) {
			c->cap_pend = c->cap_pend ? 2 * c->cap_pend : 16;
			c->pend = realloc(c->pend, c->cap_pend * sizeof(*c->pend));
		}
		struct pending *p = &c->pend[c->n_pend++];
		p->ring_idx = ring_idx;
		p->fence_id = ++c->fence_id;
		p->n = sync_count;
		p->sync = malloc(sync_count * sizeof(*p->sync));
		for (uint32_t k = 0; k < sync_count; k++) {
			const uint32_t *s = w + sync_off + 4 * k;
			p->sync[k].addr = s[0] | (uint64_t)s[1] << 32;
			p->sync[k].value = s[2] | (uint64_t)s[3] << 32;
		}
		if (!vkr_renderer_submit_fence(rec->ctx, 0, ring_idx, p->fence_id)) {
			logf("context %u: no fence on ring %u: its syncs are signalled now\n", rec->ctx, ring_idx);
			on_retire(rec->ctx, ring_idx, p->fence_id);
		}
	}
	/* The record is done once its commands ran: what the GPU does later is the syncs' and the rings' */
	rvgpu_complete(rec->ctx, rec->serial);
}

static bool
poll_vkr(void *data)
{
	(void)data;
	const bool busy = vkr_renderer_poll();
	/* RVGPU_VKR_STATS: a line every 5 s while venus works (in the page: the Worker's log) */
	static uint64_t last_t, last_cmds;
	if (V.stats) {
		const uint64_t t = vkr_now_ns(), all = vkr_ring_stats.cmds + vkr_ring_stats.nested;
		if (t - last_t > 5000000000ull && all != last_cmds) {
			fprintf(stderr, "rvgpu-vkr: %llu venus commands + %llu in command streams (%.0f KiB), %.1f ms: "
				"%.2f us each; %u submits, %u batches\n", (unsigned long long)vkr_ring_stats.cmds,
				(unsigned long long)vkr_ring_stats.nested, vkr_ring_stats.bytes / 1024.0, vkr_ring_stats.ns / 1e6,
				all ? vkr_ring_stats.ns / 1e3 / all : 0, V.submits, V.batches);
			last_t = t;
			last_cmds = all;
		}
	}
	return busy;
}

uint32_t
rvgpu_vkr_capset(void *out)
{
	return (uint32_t)vkr_get_capset(out, 0);
}

int
rvgpu_vkr_start(PFN_vkGetInstanceProcAddr gipa,
		void (*set_guest)(VkDevice dev, VkDeviceMemory mem, uint64_t guest, uint64_t size),
		unsigned (*device_poll)(VkDevice dev))
{
	static const struct vkr_renderer_callbacks cbs = { on_log, on_retire };
	V.quiet = getenv("RVGPU_VKR_QUIET") != NULL;
#ifdef __EMSCRIPTEN__
	V.stats = true;
#else
	V.stats = getenv("RVGPU_VKR_STATS") != NULL;
#endif
	vkr_renderer_set_get_instance_proc_addr(gipa);
	V.set_guest = set_guest;
	vkr_renderer_set_device_memory_guest_hook(guest_hook);
	vkr_renderer_set_device_poll_hook(device_poll);
	if (!vkr_renderer_init(VKR_RENDERER_THREAD_SYNC | VKR_RENDERER_ASYNC_FENCE_CB, &cbs)) {
		logf("vkr_renderer_init failed\n");
		return -1;
	}
	rvgpu_ring_register_front(RVGPU_FRONT_VENUS, RVGPU_OP_VN_CTX_INIT, op_ctx_init, NULL);
	rvgpu_ring_register_front(RVGPU_FRONT_VENUS, RVGPU_OP_CTX_DESTROY, op_ctx_destroy, NULL);
	rvgpu_ring_register_front(RVGPU_FRONT_VENUS, RVGPU_OP_VN_BLOB_CREATE, op_blob_create, NULL);
	rvgpu_ring_register_front(RVGPU_FRONT_VENUS, RVGPU_OP_RES_UNREF, op_res_unref, NULL);
	rvgpu_ring_register_front(RVGPU_FRONT_VENUS, RVGPU_OP_VN_SUBMIT, op_submit, NULL);
	rvgpu_ring_set_poll(poll_vkr, NULL);

	uint8_t capset[1024];
	uint32_t len = rvgpu_vkr_capset(NULL);
	if (len > sizeof(capset)) {
		logf("the venus capset is %u bytes\n", len);
		return -1;
	}
	rvgpu_vkr_capset(capset);
	rvgpu_set_venus_caps(capset, len);
	return 0;
}

#ifdef __EMSCRIPTEN__
#include "wgvk.h"

/* T2's (wgvk_cmd.c): retire what WebGPU finished; a build without T2 has none */
unsigned wgvk_device_poll(VkDevice device) __attribute__((weak));

/* The page (rvgpu-worker.js), after rvgpu_init and rvgpu_ring_attach: vkr on wgvk */
int rvgpu_vkr_page_start(void) __attribute__((used, visibility("default")));
int
rvgpu_vkr_page_start(void)
{
	return rvgpu_vkr_start((PFN_vkGetInstanceProcAddr)wgvk_GetInstanceProcAddr, wgvk_memory_set_guest_window,
			       wgvk_device_poll);
}
#endif
