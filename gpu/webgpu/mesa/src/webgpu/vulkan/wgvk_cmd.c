/*
 * wgvk command buffers and the queue (T2; INTERFACES.md 5).
 *
 * Recording: Mesa's vk_cmd_queue. Commands with an older and a newer form
 * (vkCmdCopyBuffer / vkCmdCopyBuffer2, render passes / dynamic rendering,
 * barriers) go through Mesa's common translators at record time, so the queue
 * holds one form; everything else is enqueued as it comes.
 *
 * Replay: at vkQueueSubmit every command buffer of the submission is replayed
 * into one GPUCommandEncoder (wgvk_replay_cmd_buffer below). State commands
 * (pipeline binds, dynamic state) go through Mesa's common implementations,
 * which keep vk_command_buffer::dynamic_graphics_state; actions are wgvk's:
 * render passes and draws in wgvk_render.c, transfers in wgvk_copy.c,
 * dispatches here.
 *
 * Host-visible memory: the window (the guest's pages in the page, a host
 * allocation natively) is the truth. Before the submission, the ranges its
 * commands read are written into the GPUBuffer (queue.writeBuffer, ordered
 * before the submission on the queue). The ranges the GPU writes are copied
 * to MAP_READ buffers at the end of the same command buffer, mapped when the
 * GPU is done, and copied into the window before the submission's syncs
 * signal. Until then uploads skip those ranges, so they cannot overwrite
 * what the GPU wrote with older bytes.
 *
 * Completion: every submission gets a serial; its syncs (fences, semaphores,
 * timelines) move when it completes. Nothing blocks in vkQueueSubmit. Waiting
 * (vkWaitForFences and the rest) goes through wgvk_queue_poll, which waits on
 * the WebGPU futures of the oldest submissions (wgvk_wait). A front end that
 * wants completion words (INTERFACES.md 5) sets wgvk_device::on_complete.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wgvk_replay.h"

#include "util/os_time.h"
#include "vk_format.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"
#include "vk_descriptor_update_template.h"
#include "vk_pipeline_layout.h"
#include "wgvk_remap_assign.h"

/* ---- command buffer objects */

static VkResult
cmd_buffer_create(struct vk_command_pool *pool, VkCommandBufferLevel level, struct vk_command_buffer **out)
{
	struct wgvk_cmd_buffer *cmd = vk_zalloc(&pool->alloc, sizeof(*cmd), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!cmd)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkResult r = vk_command_buffer_init_with_params(&cmd->vk, &(struct vk_command_buffer_init_params){
		.pool = pool, .ops = &wgvk_cmd_buffer_ops, .level = level, .needs_cmd_queue = true });
	if (r != VK_SUCCESS) {
		vk_free(&pool->alloc, cmd);
		return r;
	}
	cmd->vk.dynamic_graphics_state.vi = &cmd->dyn_vi;
	cmd->vk.dynamic_graphics_state.ms.sample_locations = &cmd->dyn_sl;
	*out = &cmd->vk;
	return VK_SUCCESS;
}

static void
cmd_buffer_reset(struct vk_command_buffer *cmd, VkCommandBufferResetFlags flags)
{
	vk_command_buffer_reset(cmd);
}

static void
cmd_buffer_destroy(struct vk_command_buffer *cmd)
{
	vk_command_buffer_finish(cmd);
	vk_free(&cmd->pool->alloc, cmd);
}

const struct vk_command_buffer_ops wgvk_cmd_buffer_ops = {
	.create = cmd_buffer_create,
	.reset = cmd_buffer_reset,
	.destroy = cmd_buffer_destroy,
};

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
	VK_FROM_HANDLE(wgvk_cmd_buffer, cmd, commandBuffer);
	vk_command_buffer_begin(&cmd->vk, pBeginInfo);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
	VK_FROM_HANDLE(wgvk_cmd_buffer, cmd, commandBuffer);
	return vk_command_buffer_end(&cmd->vk);
}

/* The device's dispatch table for recording: wgvk's own entry points, Mesa's translators for the commands
 * that have one form wgvk replays, and vk_cmd_queue for every other command. */
void
wgvk_record_dispatch(struct vk_device_dispatch_table *dt)
{
	vk_device_dispatch_table_from_entrypoints(dt, &wgvk_device_entrypoints, true);
	struct vk_device_dispatch_table common;
	vk_device_dispatch_table_from_entrypoints(&common, &vk_common_device_entrypoints, true);
#define XLATE(name) dt->name = common.name
	XLATE(CmdBeginRenderPass);
	XLATE(CmdBeginRenderPass2);
	XLATE(CmdNextSubpass);
	XLATE(CmdNextSubpass2);
	XLATE(CmdEndRenderPass);
	XLATE(CmdEndRenderPass2);
	XLATE(CmdEndRendering);
	XLATE(CmdCopyBuffer);
	XLATE(CmdCopyImage);
	XLATE(CmdBlitImage);
	XLATE(CmdCopyBufferToImage);
	XLATE(CmdCopyImageToBuffer);
	XLATE(CmdResolveImage);
	XLATE(CmdPipelineBarrier);
	XLATE(CmdSetEvent);
	XLATE(CmdResetEvent);
	XLATE(CmdWaitEvents);
	XLATE(CmdDispatch);
	XLATE(CmdExecuteCommands);	/* secondaries are inlined into the primary at record time */
#undef XLATE
	vk_device_dispatch_table_from_entrypoints(dt, &vk_cmd_enqueue_device_entrypoints, false);
}

/* State commands are replayed through Mesa's common implementations (they keep the command buffer's
 * dynamic graphics state, and vkCmdBindPipeline calls wgvk_cmd_bind_shaders). */
static const enum vk_cmd_type state_cmds[] = {
	VK_CMD_BIND_PIPELINE, VK_CMD_SET_VIEWPORT, VK_CMD_SET_VIEWPORT_WITH_COUNT, VK_CMD_SET_SCISSOR,
	VK_CMD_SET_SCISSOR_WITH_COUNT, VK_CMD_SET_LINE_WIDTH, VK_CMD_SET_DEPTH_BIAS, VK_CMD_SET_BLEND_CONSTANTS,
	VK_CMD_SET_DEPTH_BOUNDS, VK_CMD_SET_STENCIL_COMPARE_MASK, VK_CMD_SET_STENCIL_WRITE_MASK,
	VK_CMD_SET_STENCIL_REFERENCE, VK_CMD_SET_VERTEX_INPUT_EXT, VK_CMD_SET_CULL_MODE, VK_CMD_SET_FRONT_FACE,
	VK_CMD_SET_PRIMITIVE_TOPOLOGY, VK_CMD_SET_DEPTH_TEST_ENABLE, VK_CMD_SET_DEPTH_WRITE_ENABLE,
	VK_CMD_SET_DEPTH_COMPARE_OP, VK_CMD_SET_DEPTH_BOUNDS_TEST_ENABLE, VK_CMD_SET_STENCIL_TEST_ENABLE,
	VK_CMD_SET_STENCIL_OP, VK_CMD_SET_LINE_STIPPLE, VK_CMD_SET_DEPTH_BIAS_ENABLE, VK_CMD_SET_LOGIC_OP_EXT,
	VK_CMD_SET_PATCH_CONTROL_POINTS_EXT, VK_CMD_SET_PRIMITIVE_RESTART_ENABLE,
	VK_CMD_SET_RASTERIZER_DISCARD_ENABLE, VK_CMD_SET_COLOR_WRITE_ENABLE_EXT, VK_CMD_SET_POLYGON_MODE_EXT,
	VK_CMD_SET_DEPTH_CLAMP_ENABLE_EXT, VK_CMD_SET_DEPTH_CLIP_ENABLE_EXT, VK_CMD_SET_LOGIC_OP_ENABLE_EXT,
	VK_CMD_SET_SAMPLE_MASK_EXT, VK_CMD_SET_RASTERIZATION_SAMPLES_EXT, VK_CMD_SET_ALPHA_TO_COVERAGE_ENABLE_EXT,
	VK_CMD_SET_ALPHA_TO_ONE_ENABLE_EXT, VK_CMD_SET_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT,
	VK_CMD_SET_LINE_RASTERIZATION_MODE_EXT, VK_CMD_SET_LINE_STIPPLE_ENABLE_EXT,
	VK_CMD_SET_PROVOKING_VERTEX_MODE_EXT, VK_CMD_SET_COLOR_BLEND_ENABLE_EXT, VK_CMD_SET_COLOR_BLEND_EQUATION_EXT,
	VK_CMD_SET_COLOR_WRITE_MASK_EXT, VK_CMD_SET_TESSELLATION_DOMAIN_ORIGIN_EXT,
	VK_CMD_SET_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT, VK_CMD_SET_DEPTH_CLAMP_RANGE_EXT,
	VK_CMD_SET_PRIMITIVE_RESTART_INDEX_EXT, VK_CMD_SET_RASTERIZATION_STREAM_EXT,
	VK_CMD_SET_RENDERING_ATTACHMENT_LOCATIONS, VK_CMD_SET_RENDERING_INPUT_ATTACHMENT_INDICES,
	VK_CMD_SET_DEPTH_BIAS2_EXT,
};
static bool is_state_cmd[VK_CMD_TYPE_COUNT];

void wgvk_replay_init_dispatch(struct wgvk_device *dev);

void
wgvk_replay_init_dispatch(struct wgvk_device *dev)
{
	vk_device_dispatch_table_from_entrypoints(&dev->replay, &vk_common_device_entrypoints, true);
	for (unsigned i = 0; i < ARRAY_SIZE(state_cmds); i++)
		is_state_cmd[state_cmds[i]] = true;
}

/* One state command through the common code: a one-entry copy of the queue entry. */
static void
replay_state(struct wgvk_replay *rp, const struct vk_cmd_queue_entry *e)
{
	struct vk_cmd_queue_entry copy;
	memset(&copy, 0, sizeof(copy));
	memcpy(&copy, e, MIN2(vk_cmd_queue_type_sizes[e->type], sizeof(copy)));
	struct vk_cmd_queue q;
	list_inithead(&q.cmds);
	list_addtail(&copy.cmd_link, &q.cmds);
	vk_cmd_queue_execute(&q, wgvk_cmd_buffer_to_handle(rp->cmd), &rp->dev->replay);
}

/* vk_device_shader_ops::cmd_bind_shaders, called while a vkCmdBindPipeline is replayed. */
void wgvk_cmd_bind_shaders(struct vk_command_buffer *vk_cmd, uint32_t count, const mesa_shader_stage *stages,
			   struct vk_shader **const shaders);
void
wgvk_cmd_bind_shaders(struct vk_command_buffer *vk_cmd, uint32_t count, const mesa_shader_stage *stages,
		      struct vk_shader **const shaders)
{
	struct wgvk_cmd_buffer *cmd = container_of(vk_cmd, struct wgvk_cmd_buffer, vk);
	struct wgvk_replay *rp = cmd->rp;
	if (!rp)
		return;
	for (uint32_t i = 0; i < count; i++) {
		struct wgvk_shader *s = shaders[i] ? container_of(shaders[i], struct wgvk_shader, vk) : NULL;
		switch (stages[i]) {
		case MESA_SHADER_VERTEX: rp->vs = s; break;
		case MESA_SHADER_FRAGMENT: rp->fs = s; break;
		case MESA_SHADER_GEOMETRY: rp->gs = s; break;
		case MESA_SHADER_COMPUTE: rp->cs = s; break;
		default:
			if (s)
				wgvk_warn_once("%s shader bound: needs T4\n", _mesa_shader_stage_to_string(stages[i]));
			break;
		}
	}
}

/* ---- host-visible memory: upload on use, download before completion */

extern void rvgpu_guest_upload_buffer(WGPUQueue q, WGPUBuffer b, uint64_t boff, uint64_t addr, size_t len)
	__attribute__((weak));
extern void rvgpu_guest_from_mapped(WGPUBuffer mapped, uint64_t boff, uint64_t addr, size_t len) __attribute__((weak));

static void
upload_range(struct wgvk_replay *rp, struct wgvk_device_memory *mem, uint64_t s, uint64_t e)
{
	if (e <= s)
		return;
	struct wgvk_device *dev = rp->dev;
	if (mem->bo.window.kind == WGVK_WINDOW_HOST)
		wgpuQueueWriteBuffer(dev->wqueue, mem->bo.buffer, mem->bo.offset + s, (char *)mem->bo.window.host + s, e - s);
	else if (mem->bo.window.kind == WGVK_WINDOW_GUEST && rvgpu_guest_upload_buffer)
		rvgpu_guest_upload_buffer(dev->wqueue, mem->bo.buffer, mem->bo.offset + s, mem->bo.window.guest + s, e - s);
	dev->stats.upload_bytes += e - s;
	dev->stats.uploads++;
}

/* A command reads [start, start+size) of mem: bring the window's bytes to the GPU before the submission. */
void
wgvk_upload(struct wgvk_replay *rp, struct wgvk_device_memory *mem, uint64_t start, uint64_t size)
{
	if (!mem || !(mem->bo.flags & WGVK_BO_HOST_VISIBLE) || !size || mem->bo.window.kind == WGVK_WINDOW_NONE)
		return;
	uint64_t s = start & ~3ull, e = MIN2(align64(start + size, 4), mem->bo.size);
	if (e > s)
		util_dynarray_append(&rp->uploaded, ((struct wgvk_range){ mem, s, e }));
}

static int
cmp_range(const void *a, const void *b)
{
	const struct wgvk_range *x = a, *y = b;
	if (x->mem != y->mem)
		return x->mem < y->mem ? -1 : 1;
	return x->start < y->start ? -1 : x->start > y->start;
}

void *
wgvk_ring_alloc(struct wgvk_replay *rp, uint64_t size, WGPUBuffer *buf, uint64_t *off)
{
	size = align64(MAX2(size, 16), 256);
	struct wgvk_ring_chunk *c = NULL;
	if (util_dynarray_num_elements(&rp->ring, struct wgvk_ring_chunk))
		c = util_dynarray_top_ptr(&rp->ring, struct wgvk_ring_chunk);
	if (!c || c->used + size > c->size) {
		struct wgvk_ring_chunk n = { .size = MAX2(256 * 1024, size) };
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.size = n.size;
		bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
			   WGPUBufferUsage_Index | WGPUBufferUsage_Indirect | WGPUBufferUsage_CopySrc;
		n.buf = wgpuDeviceCreateBuffer(rp->dev->wdev, &bd);
		n.cpu = calloc(1, n.size);
		util_dynarray_append(&rp->sub->buffers, n.buf);
		util_dynarray_append(&rp->ring, n);
		c = util_dynarray_top_ptr(&rp->ring, struct wgvk_ring_chunk);
	}
	*buf = c->buf;
	*off = c->used;
	void *p = c->cpu + c->used;
	c->used += size;
	return p;
}

static int vk_dyn_index(const struct wgvk_descriptor_set_layout *l, unsigned wb);

/* The buffer heap of one shader (INTERFACES 4.4, 6.1): its lowered storage buffers' descriptors (dynamic
 * offsets applied), T1's table and arena windows; the table in the driver ring. */
void
wgvk_heap_prepare(struct wgvk_replay *rp, const struct wgvk_compiled_shader *cs, const struct wgvk_layout_remap *remap,
		  const struct wgvk_bound_sets *bind, struct wgvk_heap_state *hs)
{
	memset(hs, 0, sizeof(*hs));
	hs->ready = true;
	unsigned n = cs ? cs->legal.heap_slots : 0;
	struct wgvk_descriptor tmp[64];
	const struct wgvk_descriptor *slots[64] = { 0 };
	n = MIN2(n, 64);
	for (unsigned i = 0; cs && i < cs->legal.num_lowered; i++) {
		const struct wgvk_lowered_binding *l = &cs->legal.lowered[i];
		if (l->kind != WGVK_LOWERED_HEAP || l->heap_slot >= n || l->set >= WGVK_MAX_SETS)
			continue;
		struct wgvk_descriptor_set *set = bind->set[l->set];
		const struct wgvk_binding_remap *r = wgvk_remap_find(remap, l->set, l->binding);
		if (!set || !r || r->wg_binding + l->index >= set->count)
			continue;
		tmp[l->heap_slot] = set->desc[r->wg_binding + l->index];
		if (r->flags & WGVK_REMAP_DYNAMIC) {
			int di = vk_dyn_index(set->layout, r->wg_binding + l->index);
			if (di >= 0 && di < WGVK_MAX_DYN)
				tmp[l->heap_slot].buf.offset += bind->dyn[l->set][di];
		}
		slots[l->heap_slot] = &tmp[l->heap_slot];
	}
	uint32_t *table = wgvk_ring_alloc(rp, MAX2(n, 1) * 16, &hs->table, &hs->table_off);
	hs->table_size = align64(MAX2(n, 1) * 16, 16);
	hs->narenas = wgvk_heap_build(rp->dev, slots, n, table, hs->arenas);
	for (unsigned a = 0; a < (unsigned)hs->narenas; a++)
		for (unsigned i = 0; i < n; i++)
			if (slots[i] && slots[i]->kind == WGVK_DESC_BUFFER && slots[i]->buf.buffer->mem &&
			    slots[i]->buf.buffer->mem->bo.buffer == hs->arenas[a].buffer)
				hs->mem[a] = slots[i]->buf.buffer->mem;
	rp->dev->stats.heap_draws++;
}

/* A heap binding (902 or 910 + a) of a prepared heap; its use (the arenas are read-write storage). */
bool
wgvk_heap_entry(const struct wgvk_heap_state *hs, uint32_t binding, WGPUBindGroupEntry *e, struct wgvk_use *use)
{
	if (binding == WGVK_DRIVER_BINDING_HEAP_TABLE) {
		e->buffer = hs->table;
		e->offset = hs->table_off;
		e->size = hs->table_size;
		return false;
	}
	unsigned a = binding - WGVK_DRIVER_BINDING_HEAP;
	e->buffer = hs->arenas[a].buffer;
	e->offset = hs->arenas[a].offset;
	e->size = hs->arenas[a].size;
	if (a >= (unsigned)hs->narenas || !hs->mem[a])
		return false;
	*use = (struct wgvk_use){ hs->mem[a], e->buffer, e->offset, e->size, 1, WGVK_USE_DRIVER, (uint16_t)a, -1 };
	return true;
}

/* The driver entries of a compute shader's group: push constants over the immediates (900) and the driver
 * uniforms (901, zero for compute). */
unsigned
wgvk_driver_entries_compute(struct wgvk_replay *rp, struct wgvk_shader *cs, const struct wgvk_bgl *bgl,
			    WGPUBindGroupEntry *out, struct wgvk_use *uses, unsigned *nuse)
{
	unsigned n = 0;
	struct wgvk_heap_state hs = { 0 };
	for (unsigned i = 0; i < bgl->n && n < 8; i++) {
		uint32_t b = bgl->entries[i].binding;
		if (b < WGVK_DRIVER_BINDING_BASE)
			continue;
		WGPUBindGroupEntry *e = &out[n];
		*e = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
		e->binding = b;
		if (b == WGVK_DRIVER_BINDING_PUSH_CONSTANTS || b == WGVK_DRIVER_BINDING_UNIFORMS) {
			uint64_t size = b == WGVK_DRIVER_BINDING_PUSH_CONSTANTS ? sizeof(rp->push) :
				        sizeof(struct wgvk_driver_uniforms);
			void *p = wgvk_ring_alloc(rp, size, &e->buffer, &e->offset);
			if (b == WGVK_DRIVER_BINDING_PUSH_CONSTANTS)
				memcpy(p, rp->push, size);
			e->size = size;
			n++;
		} else if (b == WGVK_DRIVER_BINDING_HEAP_TABLE ||
			   (b >= WGVK_DRIVER_BINDING_HEAP && b < WGVK_DRIVER_BINDING_HEAP + WGVK_HEAP_ARENAS)) {
			if (!hs.ready)
				wgvk_heap_prepare(rp, &cs->cs, &cs->remap, &rp->bind[1], &hs);
			struct wgvk_use u;
			if (wgvk_heap_entry(&hs, b, e, &u) && uses && *nuse < 256)
				uses[(*nuse)++] = u;	/* in the dispatch's usage scope */
			n++;
		} else {
			wgvk_warn_once("compute driver binding %u not filled yet (T2)\n", b);
			e->buffer = wgvk_dummy_buffer(rp->dev);
			e->size = 256;
			n++;
		}
	}
	return n;
}

/* The uploads of the submission, before it goes to the queue: sorted, merged (ranges closer than this are
 * one write: the bytes between are the window's too, which is the truth), and minus what the GPU has newer
 * (writes of an earlier submission still on their way back). queue.writeBuffer runs before the submission
 * on the queue, so it does not matter where in the replay a use came from. */
#define WGVK_UPLOAD_MERGE_GAP 4096

static void
flush_uploads(struct wgvk_replay *rp)
{
	util_dynarray_foreach(&rp->ring, struct wgvk_ring_chunk, c) {
		if (c->used)
			wgpuQueueWriteBuffer(rp->dev->wqueue, c->buf, 0, c->cpu, c->used);
		free(c->cpu);
		c->cpu = NULL;
	}
	unsigned n = util_dynarray_num_elements(&rp->uploaded, struct wgvk_range);
	struct wgvk_range *u = rp->uploaded.data;
	if (!n)
		return;
	qsort(u, n, sizeof(*u), cmp_range);
	for (unsigned i = 0; i < n;) {
		struct wgvk_range r = u[i++];
		while (i < n && u[i].mem == r.mem && u[i].start <= r.end + WGVK_UPLOAD_MERGE_GAP) {
			r.end = MAX2(r.end, u[i].end);
			i++;
		}
		struct wgvk_device_memory *mem = r.mem;
		if (mem->dirty_serial > rp->dev->queue.completed && mem->dirty_end > mem->dirty_start) {
			upload_range(rp, mem, r.start, MIN2(r.end, mem->dirty_start));
			upload_range(rp, mem, MAX2(r.start, mem->dirty_end), r.end);
		} else {
			upload_range(rp, mem, r.start, r.end);
		}
	}
}

/* A command writes [start, start+size) of mem: copy it back to the window when the submission is done. */
void
wgvk_wrote(struct wgvk_replay *rp, struct wgvk_device_memory *mem, uint64_t start, uint64_t size)
{
	if (!mem || !(mem->bo.flags & WGVK_BO_HOST_VISIBLE) || !size || mem->bo.window.kind == WGVK_WINDOW_NONE)
		return;
	uint64_t s = start & ~3ull, e = MIN2(align64(start + size, 4), mem->bo.size);
	util_dynarray_foreach(&rp->written, struct wgvk_range, w) {
		if (w->mem == mem && s <= w->end && e >= w->start) {
			w->start = MIN2(w->start, s);
			w->end = MAX2(w->end, e);
			return;
		}
	}
	util_dynarray_append(&rp->written, ((struct wgvk_range){ mem, s, e }));
}

/* A use of a buffer range by a command: upload if it reads, note the write if it writes. */
void
wgvk_use_buffer(struct wgvk_replay *rp, const struct wgvk_use *u)
{
	if (!u->mem)
		return;
	uint64_t moff = u->off - u->mem->bo.offset;
	/* a storage binding may read what it writes: upload it too */
	wgvk_upload(rp, u->mem, moff, u->size);
	if (u->write)
		wgvk_wrote(rp, u->mem, moff, u->size);
}

WGPUBuffer
wgvk_temp_buffer(struct wgvk_replay *rp, uint64_t size, WGPUBufferUsage usage)
{
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = align64(MAX2(size, 4), 4);
	bd.usage = usage;
	WGPUBuffer b = wgpuDeviceCreateBuffer(rp->dev->wdev, &bd);
	util_dynarray_append(&rp->sub->buffers, b);
	return b;
}

WGPUTexture
wgvk_temp_texture(struct wgvk_replay *rp, const WGPUTextureDescriptor *td)
{
	WGPUTexture t = wgpuDeviceCreateTexture(rp->dev->wdev, td);
	util_dynarray_append(&rp->sub->textures, t);
	return t;
}

/* ---- bind groups: T1 makes them from the descriptor set (wgvk_bind_group_get); the replay notes what each
 * entry uses (buffer ranges, textures) for uploads, downloads and usage scopes */

/* Vulkan's dynamic offset index of WebGPU binding wb of a set layout, or -1 */
static int
vk_dyn_index(const struct wgvk_descriptor_set_layout *l, unsigned wb)
{
	unsigned idx = 0;
	for (unsigned i = 0; i < l->n_remap; i++) {
		const struct wgvk_binding_remap *r = &l->remap[i];
		if (!(r->flags & WGVK_REMAP_DYNAMIC))
			continue;
		if (wb >= r->wg_binding && wb < r->wg_binding + r->count)
			return idx + (wb - r->wg_binding);
		idx += r->count;
	}
	return -1;
}

struct wgvk_bg *
wgvk_replay_bind_group(struct wgvk_replay *rp, struct wgvk_descriptor_set *set, const struct wgvk_bgl *bgl,
		       const WGPUBindGroupEntry *driver, unsigned ndriver)
{
	struct wgvk_device *dev = rp->dev;
	/* T1 caches bind groups by content: the same WebGPU bind group is the same uses */
	WGPUBindGroup g = wgvk_bind_group_get(dev, set, bgl, driver, ndriver);
	struct hash_entry *he = _mesa_hash_table_search(rp->bg_table, g);
	if (he)
		return he->data;
	struct wgvk_bg *bg = calloc(1, sizeof(*bg) + bgl->n * (sizeof(struct wgvk_use) + sizeof(struct wgvk_tuse)) +
				       ndriver * sizeof(WGPUBindGroupEntry));
	bg->use = (struct wgvk_use *)(bg + 1);
	bg->tuse = (struct wgvk_tuse *)(bg->use + bgl->n);
	bg->driver = (WGPUBindGroupEntry *)(bg->tuse + bgl->n);
	bg->ndriver = ndriver;
	memcpy(bg->driver, driver, ndriver * sizeof(*driver));
	bg->bgl = bgl;
	bg->set = set;
	bg->bg = g;
	wgpuBindGroupAddRef(g);	/* the cache may drop it: the submission holds it */
	util_dynarray_append(&rp->sub->groups, g);
	util_dynarray_append(&rp->bgs, bg);
	_mesa_hash_table_insert(rp->bg_table, g, bg);
	dev->stats.bind_groups++;
	for (unsigned i = 0; i < bgl->n; i++) {
		const WGPUBindGroupLayoutEntry *le = &bgl->entries[i];
		const struct wgvk_descriptor *d = set && le->binding < set->count ? &set->desc[le->binding] : NULL;
		if (le->buffer.type != WGPUBufferBindingType_BindingNotUsed &&
		    le->buffer.type != WGPUBufferBindingType_Undefined) {
			struct wgvk_buffer *b = NULL;
			uint64_t off = 0, range = 0;
			if (d && d->kind == WGVK_DESC_BUFFER) {
				b = d->buf.buffer;
				off = d->buf.offset;
				range = d->buf.range;
			} else if (d && d->kind == WGVK_DESC_TEXEL && d->texel.view->vk.buffer) {
				b = container_of(d->texel.view->vk.buffer, struct wgvk_buffer, vk);
				off = d->texel.view->vk.offset;
				range = d->texel.view->vk.range;
			} else if (d && d->kind == WGVK_DESC_IMAGE && d->img.view &&
				   le->buffer.type != WGPUBufferBindingType_Uniform) {
				/* an image buffer (T3b's IMAGE_BUFFER): its texels come and go with the uses */
				struct wgvk_image_view *iv = d->img.view;
				bg->tuse[bg->ntuse++] = (struct wgvk_tuse){
					.tex = NULL, .mip0 = iv->vk.base_mip_level, .nmip = 1,
					.layer0 = iv->vk.base_array_layer, .nlayer = iv->vk.layer_count,
					.write = 1, .idx = i, .iv = iv, .ibuf = 1,
				};
				continue;
			}
			int dyn = -1;
			if (le->buffer.hasDynamicOffset) {
				int vi = set ? vk_dyn_index(set->layout, le->binding) : -1;
				if (bg->ndyn < WGVK_MAX_DYN)
					bg->dyn_vk[bg->ndyn] = vi < 0 ? 0 : vi;
				dyn = bg->ndyn++;
			}
			if (!b || !b->mem || !b->mem->bo.buffer)
				continue;	/* a dummy or the set's inline buffer: nothing to track */
			uint64_t wo;
			WGPUBuffer wb = wgvk_buffer_wgpu(b, off, &wo);
			bg->use[bg->nuse++] = (struct wgvk_use){
				.mem = b->mem, .buf = wb, .off = wo, .size = align64(range, 4),
				.write = le->buffer.type == WGPUBufferBindingType_Storage, .idx = i, .dyn = dyn,
			};
		} else if (d && d->kind == WGVK_DESC_IMAGE && d->img.view) {
			bool storage = le->storageTexture.access != WGPUStorageTextureAccess_BindingNotUsed &&
				       le->storageTexture.access != WGPUStorageTextureAccess_Undefined;
			struct wgvk_image_view *iv = d->img.view;
			struct wgvk_image *img = container_of(iv->vk.image, struct wgvk_image, vk);
			bg->tuse[bg->ntuse++] = (struct wgvk_tuse){
				.tex = img->tex, .mip0 = iv->vk.base_mip_level, .nmip = iv->vk.level_count,
				.layer0 = iv->vk.base_array_layer, .nlayer = iv->vk.layer_count,
				.write = storage && le->storageTexture.access != WGPUStorageTextureAccess_ReadOnly,
				.idx = i, .iv = iv,
			};
		}
	}
	return bg;
}

/* The dynamic offsets for setBindGroup, in WebGPU's order, from the set's Vulkan dynamic offsets. */
uint32_t
wgvk_bind_group_dyn(const struct wgvk_bg *bg, const uint32_t *vk_dyn, uint32_t *out)
{
	uint32_t n = MIN2(bg->ndyn, WGVK_MAX_DYN);
	for (uint32_t i = 0; i < n; i++)
		out[i] = vk_dyn[bg->dyn_vk[i]];
	return n;
}

/* The same bind group with some buffer entries replaced (snapshots); bufs/offs indexed like bg->use. The
 * caller releases it. */
WGPUBindGroup
wgvk_bind_group_rebuild(struct wgvk_replay *rp, const struct wgvk_bg *bg, const WGPUBuffer *bufs, const uint64_t *offs,
			const WGPUTextureView *views)
{
	WGPUBindGroupEntry *e = malloc(MAX2(bg->bgl->n, 1) * sizeof(*e));
	wgvk_bind_group_entries(rp->dev, bg->set, bg->bgl, bg->driver, bg->ndriver, e);
	for (unsigned i = 0; bufs && i < bg->nuse; i++) {
		if (!bufs[i])
			continue;
		e[bg->use[i].idx].buffer = bufs[i];
		e[bg->use[i].idx].offset = offs[i];
	}
	for (unsigned i = 0; views && i < bg->bgl->n; i++)
		if (views[i])
			e[i].textureView = views[i];
	WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bd.layout = bg->bgl->layout;
	bd.entryCount = bg->bgl->n;
	bd.entries = e;
	WGPUBindGroup g = wgpuDeviceCreateBindGroup(rp->dev->wdev, &bd);
	free(e);
	return g;
}

/* ---- descriptor binding commands */

static void
bind_sets(struct wgvk_replay *rp, VkShaderStageFlags stages, VkPipelineLayout layout, uint32_t first,
	  uint32_t count, const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn)
{
	uint32_t d = 0;
	for (uint32_t i = 0; i < count && first + i < WGVK_MAX_SETS; i++) {
		struct wgvk_descriptor_set *set = wgvk_descriptor_set_from_handle(sets[i]);
		uint32_t n = set ? set->layout->dynamic_count : 0;
		for (unsigned bp = 0; bp < 2; bp++) {
			bool want = bp == 0 ? (stages & VK_SHADER_STAGE_ALL_GRAPHICS) : (stages & VK_SHADER_STAGE_COMPUTE_BIT);
			if (!want)
				continue;
			rp->bind[bp].set[first + i] = set;
			for (uint32_t j = 0; j < n && j < WGVK_MAX_DYN && d + j < ndyn; j++)
				rp->bind[bp].dyn[first + i][j] = dyn[d + j];
		}
		d += n;
	}
}

static VkShaderStageFlags
bind_point_stages(VkPipelineBindPoint bp)
{
	return bp == VK_PIPELINE_BIND_POINT_COMPUTE ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS;
}

/* Push descriptors: a set of the replay's own; each push makes a new one (earlier draws keep theirs). */
static struct wgvk_descriptor_set *
push_set(struct wgvk_replay *rp, VkShaderStageFlags stages, VkPipelineLayout layout, uint32_t set_index)
{
	VK_FROM_HANDLE(vk_pipeline_layout, pl, layout);
	if (set_index >= WGVK_MAX_SETS || set_index >= pl->set_count || !pl->set_layouts[set_index])
		return NULL;
	struct wgvk_descriptor_set_layout *l =
		container_of(pl->set_layouts[set_index], struct wgvk_descriptor_set_layout, vk);
	struct wgvk_descriptor_set *set = wgvk_descriptor_set_create(rp->dev, l);
	unsigned bp = (stages & VK_SHADER_STAGE_COMPUTE_BIT) ? 1 : 0;
	struct wgvk_descriptor_set *old = rp->bind[bp].set[set_index];
	if (old && old->layout == l) {
		/* a push updates some descriptors: the rest are what the last push left */
		memcpy(set->desc, old->desc, set->count * sizeof(set->desc[0]));
		if (l->inline_size && set->inline_data && old->inline_data) {
			memcpy(set->inline_data, old->inline_data, l->inline_size);
			set->inline_dirty = true;
		}
	}
	util_dynarray_append(&rp->sub->sets, set);
	if (stages & VK_SHADER_STAGE_ALL_GRAPHICS)
		rp->bind[0].set[set_index] = set;
	if (stages & VK_SHADER_STAGE_COMPUTE_BIT)
		rp->bind[1].set[set_index] = set;
	return set;
}


/* ---- compute */

void
wgvk_end_compute(struct wgvk_replay *rp)
{
	if (!rp->cpass)
		return;
	wgpuComputePassEncoderEnd(rp->cpass);
	wgpuComputePassEncoderRelease(rp->cpass);
	rp->cpass = NULL;
}

/* The mip levels a texture binding covers: a storage view is one level (WebGPU), a sampled one the view's. */
static uint32_t
tuse_mips(const struct wgvk_bg *bg, const struct wgvk_tuse *t)
{
	const WGPUBindGroupLayoutEntry *le = &bg->bgl->entries[t->idx];
	bool storage = le->storageTexture.access != WGPUStorageTextureAccess_BindingNotUsed &&
		       le->storageTexture.access != WGPUStorageTextureAccess_Undefined;
	return storage ? 1 : t->nmip;
}

/* A dispatch that reads a texture range (sampled or read-only storage) it also writes as storage — mip
 * chains made level by level (Blender: HiZ, probe and shadow downsampling) — breaks WebGPU's usage scope:
 * the reads get a copy of the range made before the compute pass. views[g][entry] receives the copies'
 * views; the result says whether any was made. */
static bool
dispatch_texture_snapshots(struct wgvk_replay *rp, struct wgvk_bg *const *bg, unsigned ngroups,
			   WGPUTextureView views[WGVK_MAX_SETS][128])
{
	struct wgvk_device *dev = rp->dev;
	bool any = false;
	for (unsigned g = 0; g < ngroups; g++) {
		for (unsigned i = 0; bg[g] && i < bg[g]->ntuse; i++) {
			const struct wgvk_tuse *t = &bg[g]->tuse[i];
			if (t->write || !t->iv || t->ibuf || t->idx >= 128)
				continue;
			uint32_t tm = tuse_mips(bg[g], t);
			bool conflict = false;
			for (unsigned h = 0; h < ngroups && !conflict; h++) {
				for (unsigned j = 0; bg[h] && j < bg[h]->ntuse; j++) {
					const struct wgvk_tuse *w = &bg[h]->tuse[j];
					if (!w->write || w->ibuf || w->tex != t->tex)
						continue;
					uint32_t wm = tuse_mips(bg[h], w);
					if (w->mip0 < t->mip0 + tm && t->mip0 < w->mip0 + wm && w->layer0 < t->layer0 + t->nlayer &&
					    t->layer0 < w->layer0 + w->nlayer) {
						conflict = true;
						break;
					}
				}
			}
			if (!conflict)
				continue;
			struct wgvk_image_view *iv = t->iv;
			struct wgvk_image *img = container_of(iv->vk.image, struct wgvk_image, vk);
			if (vk_format_is_depth_or_stencil(img->vk.format) || img->vk.samples > 1) {
				wgvk_warn_once("a dispatch reads a depth or multisampled texture it writes: not snapshotted\n");
				continue;
			}
			const WGPUBindGroupLayoutEntry *le = &bg[g]->bgl->entries[t->idx];
			bool storage = le->storageTexture.access != WGPUStorageTextureAccess_BindingNotUsed &&
				       le->storageTexture.access != WGPUStorageTextureAccess_Undefined;
			WGPUTexture src = iv->alias >= 0 ? img->alias[iv->alias].tex : img->tex;
			bool is3d = img->vk.image_type == VK_IMAGE_TYPE_3D;
			WGPUTextureFormat vf = iv->wformat, tf = wgpuTextureGetFormat(src);
			if (!any)
				wgvk_end_compute(rp);
			any = true;
			WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
			td.usage = WGPUTextureUsage_CopyDst | (storage ? WGPUTextureUsage_StorageBinding : WGPUTextureUsage_TextureBinding);
			td.dimension = img->wdim;
			td.size = (WGPUExtent3D){ MAX2(img->vk.extent.width >> t->mip0, 1), MAX2(img->vk.extent.height >> t->mip0, 1),
						  is3d ? MAX2(img->vk.extent.depth >> t->mip0, 1) : t->nlayer };
			td.format = tf;
			td.mipLevelCount = tm;
			if (vf != tf) {
				td.viewFormatCount = 1;
				td.viewFormats = &vf;
			}
			WGPUTexture tmp = wgvk_temp_texture(rp, &td);
			for (uint32_t m = 0; m < tm; m++) {
				WGPUTexelCopyTextureInfo s = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, d = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
				s.texture = src;
				s.mipLevel = t->mip0 + m;
				s.origin.z = is3d ? 0 : t->layer0;
				d.texture = tmp;
				d.mipLevel = m;
				WGPUExtent3D ext = { MAX2(td.size.width >> m, 1), MAX2(td.size.height >> m, 1),
						     is3d ? MAX2(td.size.depthOrArrayLayers >> m, 1) : t->nlayer };
				wgpuCommandEncoderCopyTextureToTexture(rp->enc, &s, &d, &ext);
			}
			WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
			vd.format = vf;
			vd.dimension = storage ? le->storageTexture.viewDimension : le->texture.viewDimension;
			vd.aspect = iv->aspect;
			if (views[g][t->idx])
				wgpuTextureViewRelease(views[g][t->idx]);
			views[g][t->idx] = wgpuTextureCreateView(tmp, &vd);
			dev->stats.texture_snapshots++;
		}
	}
	return any;
}

static void
dispatch(struct wgvk_replay *rp, const uint32_t base[3], const uint32_t groups[3], struct wgvk_buffer *ind,
	 uint64_t ind_off)
{
	struct wgvk_device *dev = rp->dev;
	struct wgvk_shader *cs = rp->cs;
	if (!cs || wgvk_compute_pipeline_get(dev, cs) != VK_SUCCESS) {
		wgvk_warn_once("dispatch without a compute pipeline\n");
		return;
	}
	if (base && (base[0] || base[1] || base[2]))
		wgvk_warn_once("vkCmdDispatchBase with a base group: needs the base in immediates (T3b)\n");

	/* textures WebGPU cannot filter sampled as float: a pipeline variant with unfilterable layout entries */
	WGPUComputePipeline cpipe = cs->cpipe;
	struct wgvk_bgl *const *cbgl = cs->cbgl;
	uint32_t cgroups = cs->cgroups;
	struct wgvk_unfilterable uf;
	const struct wgvk_compiled_shader *one[1] = { &cs->cs };
	wgvk_unfilterable(&rp->bind[1], one, 1, &uf);
	if (uf.n) {
		const struct wgvk_cpipe *cp = wgvk_cpipe_get(dev, &cs->cs, &cs->remap, &uf);
		cpipe = cp->pipe;
		cbgl = cp->bgl;
		cgroups = cp->ngroups;
	}

	/* This dispatch's usage scope: its bind groups (and the indirect buffer). */
	struct wgvk_bg *bg[WGVK_MAX_SETS] = { 0 };
	uint32_t ndyn[WGVK_MAX_SETS] = { 0 }, dyn[WGVK_MAX_SETS][WGVK_MAX_DYN];
	struct wgvk_use uses[256];
	unsigned nuse = 0;
	for (unsigned g = 0; g < cgroups; g++) {
		WGPUBindGroupEntry drv[8];
		unsigned nd = wgvk_driver_entries_compute(rp, cs, cbgl[g], drv, uses, &nuse);
		bg[g] = wgvk_replay_bind_group(rp, rp->bind[1].set[g], cbgl[g], drv, nd);
		ndyn[g] = wgvk_bind_group_dyn(bg[g], rp->bind[1].dyn[g], dyn[g]);
		for (unsigned i = 0; i < bg[g]->nuse && nuse < ARRAY_SIZE(uses); i++) {
			struct wgvk_use u = bg[g]->use[i];
			if (u.dyn >= 0)
				u.off += dyn[g][u.dyn];
			u.slot = g;
			u.idx = i;
			uses[nuse++] = u;
		}
	}
	WGPUBuffer ibuf = NULL;
	uint64_t ioff = 0;
	if (ind) {
		ibuf = wgvk_buffer_wgpu(ind, ind_off, &ioff);
		uses[nuse++] = (struct wgvk_use){ ind->mem, ibuf, ioff, 12, 0, WGVK_USE_INDIRECT, 0, -1 };
	}
	for (unsigned i = 0; i < nuse; i++)
		wgvk_use_buffer(rp, &uses[i]);

	/* A buffer both written and read by this one dispatch breaks WebGPU's usage scope (one GPUBuffer per
	 * VkDeviceMemory: suballocated UBOs and SSBOs meet here): the reads get a snapshot copy. */
	WGPUBuffer snap[WGVK_MAX_SETS][64] = { 0 };
	uint64_t snap_off[WGVK_MAX_SETS][64] = { 0 };
	bool any_snap = false;
	for (unsigned i = 0; i < nuse; i++) {
		if (uses[i].write)
			continue;
		bool conflict = false;
		for (unsigned j = 0; j < nuse; j++)
			conflict |= uses[j].write && uses[j].buf == uses[i].buf;
		if (!conflict)
			continue;
		if (!any_snap)
			wgvk_end_compute(rp);
		any_snap = true;
		uint64_t tsize = uses[i].size;
		if (uses[i].slot < WGVK_MAX_SETS && bg[uses[i].slot] && uses[i].idx < bg[uses[i].slot]->nuse)
			tsize = MAX2(tsize, bg[uses[i].slot]->bgl->entries[bg[uses[i].slot]->use[uses[i].idx].idx].buffer.minBindingSize);
		WGPUBuffer t = wgvk_temp_buffer(rp, tsize, WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform |
								  WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect);
		wgpuCommandEncoderCopyBufferToBuffer(rp->enc, uses[i].buf, uses[i].off, t, 0, align64(uses[i].size, 4));
		dev->stats.snapshots++;
		dev->stats.snapshot_bytes += uses[i].size;
		if (uses[i].slot == WGVK_USE_DRIVER)
			continue;
		if (uses[i].slot < WGVK_MAX_SETS) {
			if (uses[i].idx >= 64)
				continue;
			snap[uses[i].slot][uses[i].idx] = t;
			/* the dynamic offset still applies: put the copy where offset + dynamic offset lands */
			snap_off[uses[i].slot][uses[i].idx] = 0;
		} else {
			ibuf = t;
			ioff = 0;
		}
	}

	/* image buffers and alias textures brought up to date (copies: outside the compute pass) */
	bool sync = false;
	for (unsigned g = 0; g < cgroups && !sync; g++)
		sync = bg[g] && wgvk_tuses_need_sync(bg[g]->tuse, bg[g]->ntuse);
	if (sync)
		wgvk_end_compute(rp);
	for (unsigned g = 0; g < cgroups; g++)
		if (bg[g])
			wgvk_tuses_before(rp, bg[g]->tuse, bg[g]->ntuse);

	WGPUTextureView tviews[WGVK_MAX_SETS][128] = { 0 };
	bool any_tsnap = dispatch_texture_snapshots(rp, bg, cgroups, tviews);

	if (!rp->cpass) {
		rp->cpass = wgpuCommandEncoderBeginComputePass(rp->enc, NULL);
		dev->stats.compute_passes++;
	}
	wgpuComputePassEncoderSetPipeline(rp->cpass, cpipe);
	for (unsigned g = 0; g < cgroups; g++) {
		if (!bg[g])
			continue;
		bool snapped = false;
		for (unsigned i = 0; i < bg[g]->nuse; i++)
			snapped |= snap[g][i] != NULL;
		bool tsnapped = false;
		for (unsigned i = 0; any_tsnap && i < 128; i++)
			tsnapped |= tviews[g][i] != NULL;
		if (snapped || tsnapped) {
			/* a snapshot holds the bytes at offset + dynamic offset: bind it with a zero dynamic offset */
			uint32_t d2[WGVK_MAX_DYN];
			memcpy(d2, dyn[g], sizeof(d2));
			for (unsigned i = 0; i < bg[g]->nuse; i++)
				if (snap[g][i] && bg[g]->use[i].dyn >= 0)
					d2[bg[g]->use[i].dyn] = 0;
			WGPUBindGroup sg = wgvk_bind_group_rebuild(rp, bg[g], snapped ? snap[g] : NULL, snap_off[g],
								   tsnapped ? tviews[g] : NULL);
			wgpuComputePassEncoderSetBindGroup(rp->cpass, g, sg, ndyn[g], d2);
			wgpuBindGroupRelease(sg);
			for (unsigned i = 0; tsnapped && i < 128; i++)
				if (tviews[g][i])
					wgpuTextureViewRelease(tviews[g][i]);
		} else {
			wgpuComputePassEncoderSetBindGroup(rp->cpass, g, bg[g]->bg, ndyn[g], dyn[g]);
		}
	}
	if (cs->cs.immediate_bytes)
		wgpuComputePassEncoderSetImmediates(rp->cpass, 0, rp->push, align(cs->cs.immediate_bytes, 4));
	if (ibuf)
		wgpuComputePassEncoderDispatchWorkgroupsIndirect(rp->cpass, ibuf, ioff);
	else
		wgpuComputePassEncoderDispatchWorkgroups(rp->cpass, groups[0], groups[1], groups[2]);
	dev->stats.dispatches++;
}

/* ---- indirect draws */

extern void rvgpu_guest_read(void *dst, uint64_t addr, size_t len) __attribute__((weak));

/* Bytes of a buffer as the GPU would read them at this point of the submission, if the CPU has them: the
 * memory is host-visible, and neither this submission so far nor one in flight writes the range. */
bool
wgvk_cpu_bytes(struct wgvk_replay *rp, struct wgvk_buffer *buf, uint64_t off, uint64_t size, void *out)
{
	struct wgvk_device_memory *m = buf ? buf->mem : NULL;
	if (!m || !(m->bo.flags & WGVK_BO_HOST_VISIBLE))
		return false;
	uint64_t s = buf->offset + off, e = s + size;
	if (e > m->bo.size)
		return false;
	if (m->dirty_serial > rp->dev->queue.completed && s < m->dirty_end && e > m->dirty_start)
		return false;
	util_dynarray_foreach(&rp->written, struct wgvk_range, w)
		if (w->mem == m && s < w->end && e > w->start)
			return false;
	if (m->bo.window.kind == WGVK_WINDOW_HOST)
		memcpy(out, (const char *)m->bo.window.host + s, size);
	else if (m->bo.window.kind == WGVK_WINDOW_GUEST && rvgpu_guest_read)
		rvgpu_guest_read(out, m->bo.window.guest + s, size);
	else
		return false;
	return true;
}

/* Indirect draws. When the arguments (and the count) are on the CPU, they become direct draws: exact draw
 * parameters, any draw count, no indirect buffer in the pass's usage scope. Otherwise WebGPU's indirect
 * draws, one per draw (multi-draw-indirect is not in WebGPU without flags); a GPU-written count is not
 * done yet (needs a compute pass that zeroes the draws past it). */
static void
draw_indirect(struct wgvk_replay *rp, bool indexed, struct wgvk_buffer *buf, uint64_t off, uint32_t count,
	      uint32_t stride, struct wgvk_buffer *count_buf, uint64_t count_off)
{
	uint32_t words = indexed ? 5 : 4;
	stride = stride ? stride : words * 4;
	if (count_buf) {
		uint32_t n;
		if (!wgvk_cpu_bytes(rp, count_buf, count_off, 4, &n)) {
			wgvk_warn_once("indirect draw count written by the GPU: needs a compute pre-pass (T2), max count drawn\n");
			n = count;
		}
		count = MIN2(count, n);
	}
	for (uint32_t i = 0; i < count; i++) {
		rp->draw_id = i;
		uint32_t a[5] = { 0 };
		uint64_t o = off + (uint64_t)i * stride;
		if (wgvk_cpu_bytes(rp, buf, o, words * 4, a))
			wgvk_cmd_draw(rp, indexed ? WGVK_DRAW_INDEXED : WGVK_DRAW, a, NULL, 0);
		else
			wgvk_cmd_draw(rp, indexed ? WGVK_DRAW_INDEXED_INDIRECT : WGVK_DRAW_INDIRECT, a, buf, o);
	}
	rp->draw_id = 0;
}

/* ---- the replay loop */

static void
replay_cmd_buffer(struct wgvk_replay *rp, struct wgvk_cmd_buffer *cmd)
{
	struct wgvk_device *dev = rp->dev;
	rp->cmd = cmd;
	cmd->rp = rp;
	/* Vulkan state does not carry over from one command buffer to the next */
	vk_dynamic_graphics_state_clear(&cmd->vk.dynamic_graphics_state);
	rp->vs = rp->fs = rp->cs = rp->gs = NULL;
	memset(rp->bind, 0, sizeof(rp->bind));
	memset(rp->vb, 0, sizeof(rp->vb));
	memset(&rp->ib, 0, sizeof(rp->ib));
	dev->stats.cmd_buffers++;

	static int trace = -1;
	if (trace < 0)
		trace = getenv("WGVK_TRACE") != NULL;
	list_for_each_entry(struct vk_cmd_queue_entry, e, &cmd->vk.cmd_queue.cmds, cmd_link) {
		if (trace)
			wgvk_log("trace: %s\n", vk_cmd_queue_type_names[e->type]);
		if (is_state_cmd[e->type]) {
			replay_state(rp, e);
			continue;
		}
		switch (e->type) {
		/* render passes and draws */
		case VK_CMD_BEGIN_RENDERING:
			wgvk_end_compute(rp);
			wgvk_cmd_begin_rendering(rp, e->u.begin_rendering.rendering_info);
			break;
		case VK_CMD_END_RENDERING:
		case VK_CMD_END_RENDERING2_KHR:
			wgvk_cmd_end_rendering(rp);
			break;
		case VK_CMD_DRAW: {
			struct vk_cmd_draw *c = &e->u.draw;
			wgvk_cmd_draw(rp, WGVK_DRAW,
				      (uint32_t[5]){ c->vertex_count, c->instance_count, c->first_vertex, c->first_instance },
				      NULL, 0);
			break;
		}
		case VK_CMD_DRAW_INDEXED: {
			struct vk_cmd_draw_indexed *c = &e->u.draw_indexed;
			wgvk_cmd_draw(rp, WGVK_DRAW_INDEXED,
				      (uint32_t[5]){ c->index_count, c->instance_count, c->first_index,
						     (uint32_t)c->vertex_offset, c->first_instance },
				      NULL, 0);
			break;
		}
		case VK_CMD_DRAW_MULTI_EXT: {
			struct vk_cmd_draw_multi_ext *c = &e->u.draw_multi_ext;
			for (uint32_t i = 0; i < c->draw_count; i++) {
				rp->draw_id = i;
				const VkMultiDrawInfoEXT *d = (const void *)((const char *)c->vertex_info + i * c->stride);
				wgvk_cmd_draw(rp, WGVK_DRAW,
					      (uint32_t[5]){ d->vertexCount, c->instance_count, d->firstVertex,
							     c->first_instance },
					      NULL, 0);
			}
			rp->draw_id = 0;
			break;
		}
		case VK_CMD_DRAW_MULTI_INDEXED_EXT: {
			struct vk_cmd_draw_multi_indexed_ext *c = &e->u.draw_multi_indexed_ext;
			for (uint32_t i = 0; i < c->draw_count; i++) {
				rp->draw_id = i;
				const VkMultiDrawIndexedInfoEXT *d =
					(const void *)((const char *)c->index_info + i * c->stride);
				int32_t vo = c->vertex_offset ? *c->vertex_offset : d->vertexOffset;
				wgvk_cmd_draw(rp, WGVK_DRAW_INDEXED,
					      (uint32_t[5]){ d->indexCount, c->instance_count, d->firstIndex, (uint32_t)vo,
							     c->first_instance },
					      NULL, 0);
			}
			rp->draw_id = 0;
			break;
		}
		case VK_CMD_DRAW_INDIRECT:
		case VK_CMD_DRAW_INDEXED_INDIRECT: {
			bool indexed = e->type == VK_CMD_DRAW_INDEXED_INDIRECT;
			struct vk_cmd_draw_indirect *c = &e->u.draw_indirect;	/* same layout for both */
			draw_indirect(rp, indexed, wgvk_buffer_from_handle(c->buffer), c->offset, c->draw_count, c->stride,
				      NULL, 0);
			break;
		}
		case VK_CMD_DRAW_INDIRECT_COUNT:
		case VK_CMD_DRAW_INDEXED_INDIRECT_COUNT: {
			bool indexed = e->type == VK_CMD_DRAW_INDEXED_INDIRECT_COUNT;
			struct vk_cmd_draw_indirect_count *c = &e->u.draw_indirect_count;	/* same layout for both */
			draw_indirect(rp, indexed, wgvk_buffer_from_handle(c->buffer), c->offset, c->max_draw_count,
				      c->stride, wgvk_buffer_from_handle(c->count_buffer), c->count_buffer_offset);
			break;
		}
		case VK_CMD_CLEAR_ATTACHMENTS:
			wgvk_cmd_clear_attachments(rp, &e->u.clear_attachments);
			break;

		/* binding */
		case VK_CMD_BIND_DESCRIPTOR_SETS: {
			struct vk_cmd_bind_descriptor_sets *c = &e->u.bind_descriptor_sets;
			bind_sets(rp, bind_point_stages(c->pipeline_bind_point), c->layout, c->first_set,
				  c->descriptor_set_count, c->descriptor_sets, c->dynamic_offset_count, c->dynamic_offsets);
			break;
		}
		case VK_CMD_BIND_DESCRIPTOR_SETS2: {
			const VkBindDescriptorSetsInfo *c = e->u.bind_descriptor_sets2.bind_descriptor_sets_info;
			bind_sets(rp, c->stageFlags, c->layout, c->firstSet, c->descriptorSetCount, c->pDescriptorSets,
				  c->dynamicOffsetCount, c->pDynamicOffsets);
			break;
		}
		case VK_CMD_PUSH_CONSTANTS: {
			struct vk_cmd_push_constants *c = &e->u.push_constants;
			if (c->offset + c->size <= sizeof(rp->push))
				memcpy(rp->push + c->offset, c->values, c->size);
			break;
		}
		case VK_CMD_PUSH_CONSTANTS2: {
			const VkPushConstantsInfo *c = e->u.push_constants2.push_constants_info;
			if (c->offset + c->size <= sizeof(rp->push))
				memcpy(rp->push + c->offset, c->pValues, c->size);
			break;
		}
		case VK_CMD_PUSH_DESCRIPTOR_SET: {
			struct vk_cmd_push_descriptor_set *c = &e->u.push_descriptor_set;
			struct wgvk_descriptor_set *set =
				push_set(rp, bind_point_stages(c->pipeline_bind_point), c->layout, c->set);
			for (uint32_t i = 0; set && i < c->descriptor_write_count; i++)
				wgvk_descriptor_set_write(dev, set, &c->descriptor_writes[i]);
			break;
		}
		case VK_CMD_PUSH_DESCRIPTOR_SET2: {
			const VkPushDescriptorSetInfo *c = e->u.push_descriptor_set2.push_descriptor_set_info;
			struct wgvk_descriptor_set *set = push_set(rp, c->stageFlags, c->layout, c->set);
			for (uint32_t i = 0; set && i < c->descriptorWriteCount; i++)
				wgvk_descriptor_set_write(dev, set, &c->pDescriptorWrites[i]);
			break;
		}
		case VK_CMD_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE: {
			struct vk_cmd_push_descriptor_set_with_template *c = &e->u.push_descriptor_set_with_template;
			VK_FROM_HANDLE(vk_descriptor_update_template, t, c->descriptor_update_template);
			struct wgvk_descriptor_set *set = push_set(rp, bind_point_stages(t->bind_point), c->layout, c->set);
			if (set)
				wgvk_descriptor_set_write_template(dev, set, t, c->data);
			break;
		}
		case VK_CMD_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE2: {
			const VkPushDescriptorSetWithTemplateInfo *c =
				e->u.push_descriptor_set_with_template2.push_descriptor_set_with_template_info;
			VK_FROM_HANDLE(vk_descriptor_update_template, t, c->descriptorUpdateTemplate);
			struct wgvk_descriptor_set *set = push_set(rp, bind_point_stages(t->bind_point), c->layout, c->set);
			if (set)
				wgvk_descriptor_set_write_template(dev, set, t, c->pData);
			break;
		}
		case VK_CMD_BIND_VERTEX_BUFFERS: {
			struct vk_cmd_bind_vertex_buffers *c = &e->u.bind_vertex_buffers;
			for (uint32_t i = 0; i < c->binding_count; i++) {
				VK_FROM_HANDLE(wgvk_buffer, b, c->buffers[i]);
				rp->vb[c->first_binding + i] = (struct wgvk_vb){
					b, c->offsets[i], b ? vk_buffer_range(&b->vk, c->offsets[i], VK_WHOLE_SIZE) : 0 };
			}
			break;
		}
		case VK_CMD_BIND_VERTEX_BUFFERS2: {
			struct vk_cmd_bind_vertex_buffers2 *c = &e->u.bind_vertex_buffers2;
			for (uint32_t i = 0; i < c->binding_count; i++) {
				VK_FROM_HANDLE(wgvk_buffer, b, c->buffers[i]);
				uint64_t size = c->sizes ? c->sizes[i] : VK_WHOLE_SIZE;
				rp->vb[c->first_binding + i] = (struct wgvk_vb){
					b, c->offsets[i], b ? vk_buffer_range(&b->vk, c->offsets[i], size) : 0 };
			}
			if (c->strides)
				vk_cmd_set_vertex_binding_strides(&cmd->vk, c->first_binding, c->binding_count, c->strides);
			break;
		}
		case VK_CMD_BIND_INDEX_BUFFER: {
			struct vk_cmd_bind_index_buffer *c = &e->u.bind_index_buffer;
			VK_FROM_HANDLE(wgvk_buffer, b, c->buffer);
			rp->ib.buf = b;
			rp->ib.off = c->offset;
			rp->ib.size = b ? vk_buffer_range(&b->vk, c->offset, VK_WHOLE_SIZE) : 0;
			rp->ib.type = c->index_type;
			break;
		}
		case VK_CMD_BIND_INDEX_BUFFER2: {
			struct vk_cmd_bind_index_buffer2 *c = &e->u.bind_index_buffer2;
			VK_FROM_HANDLE(wgvk_buffer, b, c->buffer);
			rp->ib.buf = b;
			rp->ib.off = c->offset;
			rp->ib.size = b ? vk_buffer_range(&b->vk, c->offset, c->size) : 0;
			rp->ib.type = c->index_type;
			break;
		}

		/* compute */
		case VK_CMD_DISPATCH_BASE: {
			struct vk_cmd_dispatch_base *c = &e->u.dispatch_base;
			dispatch(rp, (uint32_t[3]){ c->base_group_x, c->base_group_y, c->base_group_z },
				 (uint32_t[3]){ c->group_count_x, c->group_count_y, c->group_count_z }, NULL, 0);
			break;
		}
		case VK_CMD_DISPATCH: {
			struct vk_cmd_dispatch *c = &e->u.dispatch;
			dispatch(rp, NULL, (uint32_t[3]){ c->group_count_x, c->group_count_y, c->group_count_z }, NULL, 0);
			break;
		}
		case VK_CMD_DISPATCH_INDIRECT: {
			struct vk_cmd_dispatch_indirect *c = &e->u.dispatch_indirect;
			dispatch(rp, NULL, NULL, wgvk_buffer_from_handle(c->buffer), c->offset);
			break;
		}

		/* transfers (outside passes: Vulkan's rule) */
		case VK_CMD_COPY_BUFFER2:
			wgvk_end_compute(rp);
			wgvk_cmd_copy_buffer(rp, e->u.copy_buffer2.copy_buffer_info);
			break;
		case VK_CMD_COPY_IMAGE_TO_BUFFER2: {
			wgvk_end_compute(rp);
			const VkCopyImageToBufferInfo2 *c = e->u.copy_image_to_buffer2.copy_image_to_buffer_info;
			wgvk_image_before(rp, wgvk_image_from_handle(c->srcImage), -1, false);
			for (uint32_t i = 0; i < c->regionCount; i++)
				wgvk_cmd_copy_buffer_image(rp, wgvk_buffer_from_handle(c->dstBuffer),
							   wgvk_image_from_handle(c->srcImage), &c->pRegions[i], true);
			break;
		}
		case VK_CMD_COPY_BUFFER_TO_IMAGE2: {
			wgvk_end_compute(rp);
			const VkCopyBufferToImageInfo2 *c = e->u.copy_buffer_to_image2.copy_buffer_to_image_info;
			wgvk_image_before(rp, wgvk_image_from_handle(c->dstImage), -1, true);
			for (uint32_t i = 0; i < c->regionCount; i++)
				wgvk_cmd_copy_buffer_image(rp, wgvk_buffer_from_handle(c->srcBuffer),
							   wgvk_image_from_handle(c->dstImage), &c->pRegions[i], false);
			break;
		}
		case VK_CMD_COPY_IMAGE2:
			wgvk_end_compute(rp);
			wgvk_image_before(rp, wgvk_image_from_handle(e->u.copy_image2.copy_image_info->srcImage), -1, false);
			wgvk_image_before(rp, wgvk_image_from_handle(e->u.copy_image2.copy_image_info->dstImage), -1, true);
			wgvk_cmd_copy_image(rp, e->u.copy_image2.copy_image_info);
			break;
		case VK_CMD_BLIT_IMAGE2:
			wgvk_end_compute(rp);
			wgvk_image_before(rp, wgvk_image_from_handle(e->u.blit_image2.blit_image_info->srcImage), -1, false);
			wgvk_image_before(rp, wgvk_image_from_handle(e->u.blit_image2.blit_image_info->dstImage), -1, true);
			wgvk_cmd_blit_image(rp, e->u.blit_image2.blit_image_info);
			break;
		case VK_CMD_RESOLVE_IMAGE2:
			wgvk_end_compute(rp);
			wgvk_image_before(rp, wgvk_image_from_handle(e->u.resolve_image2.resolve_image_info->srcImage), -1, false);
			wgvk_image_before(rp, wgvk_image_from_handle(e->u.resolve_image2.resolve_image_info->dstImage), -1, true);
			wgvk_cmd_resolve_image(rp, e->u.resolve_image2.resolve_image_info);
			break;
		case VK_CMD_FILL_BUFFER: {
			wgvk_end_compute(rp);
			struct vk_cmd_fill_buffer *c = &e->u.fill_buffer;
			wgvk_cmd_fill_buffer(rp, wgvk_buffer_from_handle(c->dst_buffer), c->dst_offset, c->size, c->data);
			break;
		}
		case VK_CMD_UPDATE_BUFFER: {
			wgvk_end_compute(rp);
			struct vk_cmd_update_buffer *c = &e->u.update_buffer;
			wgvk_cmd_update_buffer(rp, wgvk_buffer_from_handle(c->dst_buffer), c->dst_offset, c->data_size,
					       c->data);
			break;
		}
		case VK_CMD_CLEAR_COLOR_IMAGE: {
			wgvk_end_compute(rp);
			struct vk_cmd_clear_color_image *c = &e->u.clear_color_image;
			wgvk_image_before(rp, wgvk_image_from_handle(c->image), -1, true);
			wgvk_cmd_clear_color_image(rp, wgvk_image_from_handle(c->image), c->color, c->range_count, c->ranges);
			break;
		}
		case VK_CMD_CLEAR_DEPTH_STENCIL_IMAGE: {
			wgvk_end_compute(rp);
			struct vk_cmd_clear_depth_stencil_image *c = &e->u.clear_depth_stencil_image;
			wgvk_image_before(rp, wgvk_image_from_handle(c->image), -1, true);
			wgvk_cmd_clear_ds_image(rp, wgvk_image_from_handle(c->image), c->depth_stencil, c->range_count,
						c->ranges);
			break;
		}

		/* queries */
		case VK_CMD_RESET_QUERY_POOL: {
			struct vk_cmd_reset_query_pool *c = &e->u.reset_query_pool;
			wgvk_q_reset(rp, wgvk_query_pool_from_handle(c->query_pool), c->first_query, c->query_count);
			break;
		}
		case VK_CMD_BEGIN_QUERY: {
			struct vk_cmd_begin_query *c = &e->u.begin_query;
			wgvk_q_begin(rp, wgvk_query_pool_from_handle(c->query_pool), c->query);
			break;
		}
		case VK_CMD_END_QUERY: {
			struct vk_cmd_end_query *c = &e->u.end_query;
			wgvk_q_end(rp, wgvk_query_pool_from_handle(c->query_pool), c->query);
			break;
		}
		case VK_CMD_BEGIN_QUERY_INDEXED_EXT: {
			struct vk_cmd_begin_query_indexed_ext *c = &e->u.begin_query_indexed_ext;
			wgvk_q_begin(rp, wgvk_query_pool_from_handle(c->query_pool), c->query);
			break;
		}
		case VK_CMD_END_QUERY_INDEXED_EXT: {
			struct vk_cmd_end_query_indexed_ext *c = &e->u.end_query_indexed_ext;
			wgvk_q_end(rp, wgvk_query_pool_from_handle(c->query_pool), c->query);
			break;
		}
		case VK_CMD_WRITE_TIMESTAMP2: {
			struct vk_cmd_write_timestamp2 *c = &e->u.write_timestamp2;
			wgvk_q_timestamp(rp, wgvk_query_pool_from_handle(c->query_pool), c->query);
			break;
		}
		case VK_CMD_WRITE_TIMESTAMP: {
			struct vk_cmd_write_timestamp *c = &e->u.write_timestamp;
			wgvk_q_timestamp(rp, wgvk_query_pool_from_handle(c->query_pool), c->query);
			break;
		}
		case VK_CMD_COPY_QUERY_POOL_RESULTS: {
			struct vk_cmd_copy_query_pool_results *c = &e->u.copy_query_pool_results;
			wgvk_q_copy(rp, wgvk_query_pool_from_handle(c->query_pool), c->first_query, c->query_count,
				    wgvk_buffer_from_handle(c->dst_buffer), c->dst_offset, c->stride, c->flags);
			break;
		}

		/* WebGPU orders everything on one queue: barriers and events have nothing to do */
		case VK_CMD_PIPELINE_BARRIER2:
		case VK_CMD_SET_EVENT2:
		case VK_CMD_RESET_EVENT2:
		case VK_CMD_WAIT_EVENTS2:
		case VK_CMD_SET_DEVICE_MASK:
		case VK_CMD_BEGIN_DEBUG_UTILS_LABEL_EXT:
		case VK_CMD_END_DEBUG_UTILS_LABEL_EXT:
		case VK_CMD_INSERT_DEBUG_UTILS_LABEL_EXT:
			break;

		default:
			if (e->type < VK_CMD_TYPE_COUNT) {
				static bool warned[VK_CMD_TYPE_COUNT];
				if (!warned[e->type]) {
					warned[e->type] = true;
					wgvk_log("%s not implemented (T2), skipped\n", vk_cmd_queue_type_names[e->type]);
				}
			}
			dev->stats.skipped_cmds++;
			break;
		}
	}
	cmd->rp = NULL;
}

/* ---- submission and completion */

static void
on_future(struct wgvk_submission *sub, bool ok)
{
	sub->callbacks++;
	if (!ok)
		sub->failed = true;
}

static void
on_work_done(WGPUQueueWorkDoneStatus status, WGPUStringView message, void *u1, void *u2)
{
	on_future(u1, status == WGPUQueueWorkDoneStatus_Success);
}

static void
on_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void *u1, void *u2)
{
	if (status != WGPUMapAsyncStatus_Success)
		wgvk_log("mapAsync failed: %.*s\n", (int)(message.length == WGPU_STRLEN ? 0 : message.length),
			 message.data ? message.data : "");
	on_future(u1, status == WGPUMapAsyncStatus_Success);
}

VkResult
wgvk_queue_submit(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
	struct wgvk_device *dev = container_of(vk_queue->base.device, struct wgvk_device, vk);
	uint64_t t0 = os_time_get_nano();
	wgvk_queue_poll(dev, 0, 0);	/* retire what is done */
	simple_mtx_lock(&dev->mutex);

	struct wgvk_submission *sub = calloc(1, sizeof(*sub));
	sub->serial = ++dev->queue.submitted;
	util_dynarray_init(&sub->downloads, NULL);
	util_dynarray_init(&sub->buffers, NULL);
	util_dynarray_init(&sub->textures, NULL);
	util_dynarray_init(&sub->groups, NULL);
	util_dynarray_init(&sub->sets, NULL);
	util_dynarray_init(&sub->qdone, NULL);
	util_dynarray_init(&sub->qsets, NULL);

	struct wgvk_replay *rp = calloc(1, sizeof(*rp));
	rp->dev = dev;
	rp->sub = sub;
	rp->serial = sub->serial;
	util_dynarray_init(&rp->draws, NULL);
	util_dynarray_init(&rp->uses, NULL);
	util_dynarray_init(&rp->tuses, NULL);
	util_dynarray_init(&rp->written, NULL);
	util_dynarray_init(&rp->uploaded, NULL);
	util_dynarray_init(&rp->bgs, NULL);
	rp->bg_table = _mesa_pointer_hash_table_create(NULL);
	util_dynarray_init(&rp->ring, NULL);
	util_dynarray_init(&rp->prepasses, NULL);
	rp->q.active = -1;
	util_dynarray_init(&rp->q.accs, NULL);
	util_dynarray_init(&rp->q.pending_ts, NULL);
	rp->enc = wgpuDeviceCreateCommandEncoder(dev->wdev, NULL);
	for (uint32_t i = 0; i < submit->command_buffer_count; i++)
		replay_cmd_buffer(rp, container_of(submit->command_buffers[i], struct wgvk_cmd_buffer, vk));
	if (rp->r.active)
		wgvk_cmd_end_rendering(rp);	/* a pass suspended at the end of the submission */
	wgvk_end_compute(rp);
	wgvk_q_finish(rp);
	/* (before this submission's own writes are marked dirty below) */
	flush_uploads(rp);

	/* what the GPU wrote into host-visible memory goes to MAP_READ buffers, in the same command buffer */
	util_dynarray_foreach(&rp->written, struct wgvk_range, w) {
		struct wgvk_download d = { .mem = w->mem, .start = w->start, .size = w->end - w->start };
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.size = d.size;
		bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
		d.staging = wgpuDeviceCreateBuffer(dev->wdev, &bd);
		wgpuCommandEncoderCopyBufferToBuffer(rp->enc, w->mem->bo.buffer, w->mem->bo.offset + w->start, d.staging,
						     0, d.size);
		util_dynarray_append(&sub->downloads, d);
		/* until the download lands, the GPU has the newer bytes */
		struct wgvk_device_memory *m = w->mem;
		if (m->dirty_serial > dev->queue.completed && m->dirty_end > m->dirty_start) {
			m->dirty_start = MIN2(m->dirty_start, w->start);
			m->dirty_end = MAX2(m->dirty_end, w->end);
		} else {
			m->dirty_start = w->start;
			m->dirty_end = w->end;
		}
		m->dirty_serial = sub->serial;
	}

	WGPUCommandBuffer cb = wgpuCommandEncoderFinish(rp->enc, NULL);
	wgpuQueueSubmit(dev->wqueue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	wgpuCommandEncoderRelease(rp->enc);
	dev->stats.submits++;

	util_dynarray_foreach(&sub->downloads, struct wgvk_download, d) {
		WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
		ci.mode = WGPUCallbackMode_AllowProcessEvents;
		ci.callback = on_mapped;
		ci.userdata1 = sub;
		d->future = wgpuBufferMapAsync(d->staging, WGPUMapMode_Read, 0, d->size, ci);
		sub->expected++;
	}
	if (sub->qstaging) {
		WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
		ci.mode = WGPUCallbackMode_AllowProcessEvents;
		ci.callback = on_mapped;
		ci.userdata1 = sub;
		sub->qfuture = wgpuBufferMapAsync(sub->qstaging, WGPUMapMode_Read, 0, sub->qstaging_size, ci);
		sub->expected++;
	}
	WGPUQueueWorkDoneCallbackInfo wd = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
	wd.mode = WGPUCallbackMode_AllowProcessEvents;
	wd.callback = on_work_done;
	wd.userdata1 = sub;
	sub->work_done = wgpuQueueOnSubmittedWorkDone(dev->wqueue, wd);
	sub->expected++;

	for (uint32_t i = 0; i < submit->signal_count; i++)
		wgvk_sync_signal_at(dev, submit->signals[i].sync, submit->signals[i].signal_value, sub->serial);
	list_addtail(&sub->link, &dev->queue.pending);

	util_dynarray_foreach(&rp->bgs, struct wgvk_bg *, p)
		free(*p);
	util_dynarray_fini(&rp->bgs);
	_mesa_hash_table_destroy(rp->bg_table, NULL);
	util_dynarray_fini(&rp->ring);
	util_dynarray_fini(&rp->prepasses);
	util_dynarray_fini(&rp->q.accs);
	util_dynarray_fini(&rp->q.pending_ts);
	util_dynarray_fini(&rp->draws);
	util_dynarray_fini(&rp->uses);
	util_dynarray_fini(&rp->tuses);
	util_dynarray_fini(&rp->written);
	util_dynarray_fini(&rp->uploaded);
	free(rp);
	dev->stats.submit_ms += (os_time_get_nano() - t0) / 1e6;
	simple_mtx_unlock(&dev->mutex);
	return VK_SUCCESS;
}

/* ---- device-level state of the command side */

static uint32_t
u64_hash(const void *key)
{
	return (uint32_t)*(const uint64_t *)key;
}

static bool
u64_equal(const void *a, const void *b)
{
	return *(const uint64_t *)a == *(const uint64_t *)b;
}

static void print_stats(const struct wgvk_stats *s);
static struct wgvk_device *stats_dev;	/* WGVK_STATS: the device whose stats exit prints */

static void
stats_at_exit(void)
{
	if (stats_dev)
		print_stats(&stats_dev->stats);
}

#ifndef __EMSCRIPTEN__
static void file_cache_init(struct wgvk_device *dev);
#endif

/* Module-wide (the page): the pipeline cache's store and the records to preload into every new device, and
 * the list of devices wgvk_poll_all() visits. Devices are made and polled on one thread (INTERFACES 2). */
static void (*g_cache_store)(void *data, const void *record, size_t size);
static void *g_cache_store_data;
static void *g_cache_blob;
static size_t g_cache_blob_size;
static struct wgvk_device *g_devices[16];
static unsigned preload(struct wgvk_device *dev, const void *blob, size_t size);

void
wgvk_set_pipeline_cache(void (*store)(void *data, const void *record, size_t size), void *data, const void *blob,
			size_t size)
{
	g_cache_store = store;
	g_cache_store_data = data;
	free(g_cache_blob);
	g_cache_blob = NULL;
	g_cache_blob_size = 0;
	if (size) {
		g_cache_blob = malloc(size);
		memcpy(g_cache_blob, blob, size);
		g_cache_blob_size = size;
	}
}

unsigned
wgvk_poll_all(void)
{
	unsigned n = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(g_devices); i++) {
		struct wgvk_device *dev = g_devices[i];
		if (!dev)
			continue;
		wgvk_queue_poll(dev, 0, 0);
		simple_mtx_lock(&dev->mutex);
		n += list_length(&dev->queue.pending);
		simple_mtx_unlock(&dev->mutex);
	}
	return n;
}

VkResult
wgvk_device_init_t2(struct wgvk_device *dev)
{
	for (unsigned i = 0; i < ARRAY_SIZE(g_devices); i++)
		if (!g_devices[i]) {
			g_devices[i] = dev;
			break;
		}
	if (getenv("WGVK_STATS") && !stats_dev) {
		stats_dev = dev;
		atexit(stats_at_exit);
	}
	simple_mtx_init(&dev->mutex, mtx_plain);
	list_inithead(&dev->queue.pending);
	dev->modules = _mesa_hash_table_create(NULL, u64_hash, u64_equal);
	dev->pipelines = wgvk_pipeline_table_create();
	dev->meta = _mesa_hash_table_create(NULL, u64_hash, u64_equal);
	dev->cpipes = _mesa_hash_table_create(NULL, u64_hash, u64_equal);
	wgvk_replay_init_dispatch(dev);
	WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
	dev->meta_nearest = wgpuDeviceCreateSampler(dev->wdev, &sd);
	sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
	dev->meta_linear = wgpuDeviceCreateSampler(dev->wdev, &sd);
#ifndef __EMSCRIPTEN__
	file_cache_init(dev);
#endif
	if (g_cache_store) {
		dev->cache_store = g_cache_store;
		dev->cache_store_data = g_cache_store_data;
	}
	if (g_cache_blob_size) {
		unsigned n = preload(dev, g_cache_blob, g_cache_blob_size);
		if (getenv("WGVK_STATS"))
			wgvk_log("pipeline cache: %u pipelines preloaded\n", n);
	}
	return VK_SUCCESS;
}

static void
print_stats(const struct wgvk_stats *s)
{
	wgvk_log("stats: %llu submits, %llu command buffers, %llu draws, %llu dispatches\n",
		 (unsigned long long)s->submits, (unsigned long long)s->cmd_buffers, (unsigned long long)s->draws,
		 (unsigned long long)s->dispatches);
	wgvk_log("stats: %llu render passes -> %llu WebGPU passes (%llu splits on usage scopes, %llu on feedback "
		 "loops), %llu texture snapshots, %llu buffer snapshot copies (%llu bytes), %llu feedback loops, "
		 "%llu compute passes\n",
		 (unsigned long long)s->render_passes, (unsigned long long)s->pass_segments,
		 (unsigned long long)s->splits_buffer, (unsigned long long)s->splits_feedback,
		 (unsigned long long)s->texture_snapshots, (unsigned long long)s->snapshots,
		 (unsigned long long)s->snapshot_bytes, (unsigned long long)s->feedback_loops,
		 (unsigned long long)s->compute_passes);
	wgvk_log("stats: %llu pipelines made (%llu async), %llu cache hits, %llu waits (%.1f ms), %llu bind groups\n",
		 (unsigned long long)s->pipelines_created, (unsigned long long)s->pipelines_async,
		 (unsigned long long)s->pipeline_hits, (unsigned long long)s->pipeline_waits, s->pipeline_wait_ms,
		 (unsigned long long)s->bind_groups);
	wgvk_log("stats: %llu geometry shader draws, %llu draws or dispatches with a buffer heap\n",
		 (unsigned long long)s->gs_draws, (unsigned long long)s->heap_draws);
	wgvk_log("stats: %llu draws with rewritten indices (fans, provoking vertex last, 8-bit)\n",
		 (unsigned long long)s->index_rewrites);
	wgvk_log("stats: %llu pipelines preloaded from the pipeline cache\n", (unsigned long long)s->pipelines_preloaded);
	wgvk_log("stats: %llu passes without attachments (stand-in target), %llu clear values made finite, %llu image "
		 "buffer copies\n", (unsigned long long)s->dummy_attachments, (unsigned long long)s->nonfinite_clears,
		 (unsigned long long)s->ibuf_copies);
	wgvk_log("stats: %llu indirect geometry shader draws, %llu passes with early depth tests in the shader "
		 "(depth read-only), %llu splits for them\n", (unsigned long long)s->gs_indirect_draws,
		 (unsigned long long)s->early_depth_passes, (unsigned long long)s->splits_depth);
	wgvk_log("stats: %llu texture copies between view-format aliases (%llu bytes)\n",
		 (unsigned long long)s->alias_copies, (unsigned long long)s->alias_bytes);
	wgvk_log("stats: %.1f ms in vkQueueSubmit (%.2f us per draw or dispatch)\n", s->submit_ms,
		 s->draws + s->dispatches ? s->submit_ms * 1000 / (s->draws + s->dispatches) : 0);
	wgvk_log("stats: uploads %llu (%llu bytes), downloads %llu (%llu bytes), %llu commands skipped\n",
		 (unsigned long long)s->uploads, (unsigned long long)s->upload_bytes, (unsigned long long)s->downloads,
		 (unsigned long long)s->download_bytes, (unsigned long long)s->skipped_cmds);
}

void
wgvk_device_finish_t2(struct wgvk_device *dev)
{
	for (unsigned i = 0; i < ARRAY_SIZE(g_devices); i++)
		if (g_devices[i] == dev)
			g_devices[i] = NULL;
	wgvk_queue_poll(dev, dev->queue.submitted, UINT64_MAX);
	if (getenv("WGVK_STATS"))
		print_stats(&dev->stats);
	if (dev->gs_setup_state > 0)
		wgvk_compiled_shader_free(&dev->gs_setup);
	if (stats_dev == dev)
		stats_dev = NULL;
	hash_table_foreach(dev->modules, he) {
		wgpuShaderModuleRelease(he->data);
		free((void *)he->key);
	}
	hash_table_foreach(dev->pipelines, he) {
		struct wgvk_rpipe *p = he->data;
		if (p->pending)
			wgvk_wait(dev, &p->future, 1, UINT64_MAX);
		if (p->pipeline)
			wgpuRenderPipelineRelease(p->pipeline);
		free(p);
	}
	hash_table_foreach(dev->meta, he) {
		struct wgvk_meta_entry *e = he->data;
		wgpuRenderPipelineRelease(e->pipeline);
		if (e->bgl)
			wgpuBindGroupLayoutRelease(e->bgl);
		free(e);
	}
	_mesa_hash_table_destroy(dev->modules, NULL);
	_mesa_hash_table_destroy(dev->pipelines, NULL);
	_mesa_hash_table_destroy(dev->meta, NULL);
	hash_table_foreach(dev->cpipes, he) {
		struct wgvk_cpipe *p = he->data;
		wgpuComputePipelineRelease(p->pipe);
		free(p);
	}
	_mesa_hash_table_destroy(dev->cpipes, NULL);
	wgpuSamplerRelease(dev->meta_nearest);
	wgpuSamplerRelease(dev->meta_linear);
	simple_mtx_destroy(&dev->mutex);
}

/* ---- entry points for front ends (INTERFACES.md 5) */

/* Completion words: fn(data, serial) runs when submission `serial` (1, 2, ...) has completed, its downloads
 * written and its syncs signalled. */
void wgvk_device_set_completion(VkDevice device, void (*fn)(void *data, uint64_t serial), void *data);
void
wgvk_device_set_completion(VkDevice device, void (*fn)(void *data, uint64_t serial), void *data)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	dev->on_complete = fn;
	dev->on_complete_data = data;
}

/* The serial the last vkQueueSubmit got. */
uint64_t wgvk_device_last_serial(VkDevice device);
uint64_t
wgvk_device_last_serial(VkDevice device)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	return dev->queue.submitted;
}

/* Non-blocking: retire what WebGPU has finished (the page's loop calls it after its events). Returns the
 * number of submissions still in flight. */
unsigned wgvk_device_poll(VkDevice device);
unsigned
wgvk_device_poll(VkDevice device)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	wgvk_queue_poll(dev, 0, 0);
	simple_mtx_lock(&dev->mutex);
	unsigned n = list_length(&dev->queue.pending);
	simple_mtx_unlock(&dev->mutex);
	return n;
}

/* The persistent pipeline cache (INTERFACES.md 5): store(data, record, size) gets a record for every new
 * render pipeline (the page keeps them, in IndexedDB); wgvk_device_preload_pipelines() hands them back at the
 * next start (records one after the other) and starts creating them. Returns the records preloaded. */
void wgvk_device_set_pipeline_cache(VkDevice device, void (*store)(void *data, const void *record, size_t size),
				    void *data);
void
wgvk_device_set_pipeline_cache(VkDevice device, void (*store)(void *data, const void *record, size_t size), void *data)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	dev->cache_store = store;
	dev->cache_store_data = data;
}

static unsigned
preload(struct wgvk_device *dev, const void *blob, size_t size)
{
	unsigned n = 0;
	const char *at = blob;
	while (size >= 12) {
		/* every record starts with magic, version, size: one of another version is skipped */
		uint32_t h[3];
		memcpy(h, at, sizeof(h));
		if (h[0] != 0x6b766777 || h[2] < 12 || h[2] > size)
			break;
		n += wgvk_pipeline_cache_preload(dev, at, h[2]) != 0;
		at += h[2];
		size -= h[2];
	}
	return n;
}

unsigned wgvk_device_preload_pipelines(VkDevice device, const void *blob, size_t size);
unsigned
wgvk_device_preload_pipelines(VkDevice device, const void *blob, size_t size)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	simple_mtx_lock(&dev->mutex);
	unsigned n = preload(dev, blob, size);
	simple_mtx_unlock(&dev->mutex);
	return n;
}

#ifndef __EMSCRIPTEN__
/* Natively (tests): WGVK_PIPELINE_CACHE=FILE preloads FILE's records and appends new ones to it. */
static void
file_store(void *data, const void *record, size_t size)
{
	FILE *f = fopen(data, "ab");
	if (f) {
		fwrite(record, 1, size, f);
		fclose(f);
	}
}

static void
file_cache_init(struct wgvk_device *dev)
{
	const char *path = getenv("WGVK_PIPELINE_CACHE");
	if (!path)
		return;
	FILE *f = fopen(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		void *b = malloc(MAX2(size, 1));
		if (size > 0 && fread(b, 1, size, f) == (size_t)size) {
			unsigned n = preload(dev, b, size);
			if (getenv("WGVK_STATS"))
				wgvk_log("pipeline cache %s: %u pipelines preloaded\n", path, n);
		}
		free(b);
		fclose(f);
	}
	dev->cache_store = file_store;
	dev->cache_store_data = (void *)path;
}
#endif

void wgvk_device_stats_text(VkDevice device, char *buf, size_t size);
void
wgvk_device_stats_text(VkDevice device, char *buf, size_t size)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	const struct wgvk_stats *s = &dev->stats;
	snprintf(buf, size, "pipelines made %llu, preloaded %llu, cache hits %llu, waits %llu (%.1f ms); draws %llu; "
		 "submit %.2f ms", (unsigned long long)s->pipelines_created, (unsigned long long)s->pipelines_preloaded,
		 (unsigned long long)s->pipeline_hits, (unsigned long long)s->pipeline_waits, s->pipeline_wait_ms,
		 (unsigned long long)s->draws, s->submit_ms);
}

const struct wgvk_stats *wgvk_device_stats(VkDevice device);
const struct wgvk_stats *
wgvk_device_stats(VkDevice device)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	return &dev->stats;
}
