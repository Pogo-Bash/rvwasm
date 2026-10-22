/*
 * wgvk completion: submissions and syncs (T2, with T1's sync design: signals
 * pending on serials, and the on_value hook for front ends' completion
 * words). INTERFACES.md 4.6 and 5.
 *
 * wgvk_queue_submit (wgvk_cmd.c) gives every vkQueueSubmit a serial and
 * returns without waiting. A submission completes when its WebGPU futures
 * have fired: onSubmittedWorkDone, and the mapAsync of each MAP_READ copy
 * of host-visible memory the GPU wrote. Submissions retire in serial order:
 * downloads into the windows, then the syncs they signal move, then the
 * front end's hook (wgvk_device::on_complete).
 *
 * Nothing waits unless the API waits: vkWaitForFences and the like wait for
 * the futures of the one submission that satisfies them (wgvk_wait, which
 * suspends the wasm stack in the page). The futures are AllowProcessEvents:
 * wgvk_queue_poll (also wgvk_device_poll for the page's loop) processes
 * events and retires what is done.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <poll.h>
#endif

#include "wgvk_private.h"

#include "util/os_time.h"

extern void rvgpu_guest_from_mapped(WGPUBuffer mapped, uint64_t boff, uint64_t addr, size_t len) __attribute__((weak));

static void
sub_free(struct wgvk_device *dev, struct wgvk_submission *sub)
{
	util_dynarray_foreach(&sub->buffers, WGPUBuffer, b)
		wgpuBufferRelease(*b);
	util_dynarray_foreach(&sub->textures, WGPUTexture, t) {
		wgpuTextureDestroy(*t);
		wgpuTextureRelease(*t);
	}
	util_dynarray_foreach(&sub->groups, WGPUBindGroup, g)
		wgpuBindGroupRelease(*g);
	util_dynarray_foreach(&sub->sets, struct wgvk_descriptor_set *, s)
		wgvk_descriptor_set_destroy(dev, *s);
	util_dynarray_foreach(&sub->qsets, WGPUQuerySet, q) {
		wgpuQuerySetDestroy(*q);
		wgpuQuerySetRelease(*q);
	}
	util_dynarray_fini(&sub->qsets);
	util_dynarray_fini(&sub->qdone);
	util_dynarray_fini(&sub->buffers);
	util_dynarray_fini(&sub->textures);
	util_dynarray_fini(&sub->groups);
	util_dynarray_fini(&sub->sets);
	util_dynarray_fini(&sub->downloads);
	free(sub);
}

/* The oldest submission is done: its downloads into the windows, then its serial is complete. */
static void
retire(struct wgvk_device *dev, struct wgvk_submission *sub)
{
	if (dev->on_retire)
		dev->on_retire(dev->on_retire_data);
	util_dynarray_foreach(&sub->downloads, struct wgvk_download, d) {
		/* A GUEST window (a venus blob, T5): straight from the mapped range into the guest's pages, and not
		 * after GetConstMappedRange: the page's WebGPU gives a range out once (getMappedRange of an
		 * overlapping range throws), so the download never reached the guest. */
		const bool guest = d->mem->bo.window.kind == WGVK_WINDOW_GUEST && rvgpu_guest_from_mapped;
		const void *p = sub->failed || guest ? NULL : wgpuBufferGetConstMappedRange(d->staging, 0, d->size);
		if (p && d->mem->bo.window.kind == WGVK_WINDOW_HOST)
			memcpy((char *)d->mem->bo.window.host + d->start, p, d->size);
		else if (guest && !sub->failed)
			rvgpu_guest_from_mapped(d->staging, 0, d->mem->bo.window.guest + d->start, d->size);
		if (p || (guest && !sub->failed))
			wgpuBufferUnmap(d->staging);
		wgpuBufferRelease(d->staging);
		if (d->mem->dirty_serial == sub->serial)
			d->mem->dirty_start = d->mem->dirty_end = 0;
		dev->stats.download_bytes += d->size;
		dev->stats.downloads++;
	}
	dev->queue.completed = sub->serial;
	wgvk_q_retire(dev, sub);
	list_del(&sub->link);
	if (sub->failed)
		vk_device_set_lost(&dev->vk, "wgvk: a WebGPU submission failed");
	sub_free(dev, sub);
	if (dev->on_complete)
		dev->on_complete(dev->on_complete_data, dev->queue.completed);
}

static void
retire_done(struct wgvk_device *dev)
{
	list_for_each_entry_safe(struct wgvk_submission, sub, &dev->queue.pending, link) {
		if (sub->callbacks < sub->expected)
			break;
		retire(dev, sub);
	}
}

VkResult
wgvk_queue_poll(struct wgvk_device *dev, uint64_t serial, uint64_t abs_timeout_ns)
{
	simple_mtx_lock(&dev->mutex);
	wgpuInstanceProcessEvents(dev->winst);
	retire_done(dev);
	VkResult r = VK_SUCCESS;
	while (serial && dev->queue.completed < serial && !list_is_empty(&dev->queue.pending)) {
		struct wgvk_submission *head = list_first_entry(&dev->queue.pending, struct wgvk_submission, link);
		uint64_t now = os_time_get_nano();
		uint64_t rel = abs_timeout_ns == UINT64_MAX ? UINT64_MAX : abs_timeout_ns > now ? abs_timeout_ns - now : 0;
		WGPUFuture f[16];
		unsigned n = 0;
		f[n++] = head->work_done;
		if (head->qstaging)
			f[n++] = head->qfuture;
		util_dynarray_foreach(&head->downloads, struct wgvk_download, d) {
			f[n++] = d->future;
			if (n == ARRAY_SIZE(f)) {
				r = wgvk_wait(dev, f, n, rel);
				n = 0;
				if (r != VK_SUCCESS)
					break;
			}
		}
		if (r == VK_SUCCESS && n)
			r = wgvk_wait(dev, f, n, rel);
		if (r != VK_SUCCESS)
			break;
		head->callbacks = head->expected;	/* waited on every future: their callbacks have run */
		retire_done(dev);
	}
	if (serial && dev->queue.completed < serial && list_is_empty(&dev->queue.pending))
		dev->queue.completed = MIN2(serial, dev->queue.submitted);
	simple_mtx_unlock(&dev->mutex);
	return r;
}

/* ---- the sync type */

static void
set_value(struct wgvk_sync *s, uint64_t v)
{
	if (s->value == v)
		return;
	s->value = v;
	if (s->on_value)
		s->on_value(s->on_value_data, v);
}

/* Apply the pending signals whose submissions have completed. */
void
wgvk_sync_complete(struct wgvk_device *dev, struct wgvk_sync *s)
{
	unsigned k = 0;
	for (unsigned i = 0; i < s->n_pending; i++) {
		if (s->pending[i].serial <= dev->queue.completed)
			set_value(s, (s->base.flags & VK_SYNC_IS_TIMELINE) ? MAX2(s->value, s->pending[i].value) : 1);
		else
			s->pending[k++] = s->pending[i];
	}
	s->n_pending = k;
}

void
wgvk_sync_signal_at(struct wgvk_device *dev, struct vk_sync *sync, uint64_t value, uint64_t serial)
{
	struct wgvk_sync *s = container_of(sync, struct wgvk_sync, base);
	if (serial <= dev->queue.completed) {
		set_value(s, (sync->flags & VK_SYNC_IS_TIMELINE) ? MAX2(s->value, value) : 1);
		return;
	}
	if (s->n_pending == s->max_pending) {
		s->max_pending = MAX2(4, s->max_pending * 2);
		s->pending = realloc(s->pending, s->max_pending * sizeof(*s->pending));
	}
	s->pending[s->n_pending++] = (struct wgvk_sync_signal){ value, serial };
}

static VkResult
sync_init(struct vk_device *device, struct vk_sync *sync, uint64_t initial_value)
{
	struct wgvk_sync *s = container_of(sync, struct wgvk_sync, base);
	s->value = initial_value;
	s->pending = NULL;
	s->n_pending = s->max_pending = 0;
	s->on_value = NULL;
	s->on_value_data = NULL;
	return VK_SUCCESS;
}

static void
sync_finish(struct vk_device *device, struct vk_sync *sync)
{
	free(container_of(sync, struct wgvk_sync, base)->pending);
}

static VkResult
sync_signal(struct vk_device *device, struct vk_sync *sync, uint64_t value)
{
	struct wgvk_sync *s = container_of(sync, struct wgvk_sync, base);
	set_value(s, (sync->flags & VK_SYNC_IS_TIMELINE) ? value : 1);
	return VK_SUCCESS;
}

static VkResult
sync_get_value(struct vk_device *device, struct vk_sync *sync, uint64_t *value)
{
	struct wgvk_device *dev = container_of(device, struct wgvk_device, vk);
	struct wgvk_sync *s = container_of(sync, struct wgvk_sync, base);
	wgvk_queue_poll(dev, 0, 0);
	wgvk_sync_complete(dev, s);
	*value = s->value;
	return VK_SUCCESS;
}

static VkResult
sync_reset(struct vk_device *device, struct vk_sync *sync)
{
	struct wgvk_sync *s = container_of(sync, struct wgvk_sync, base);
	s->n_pending = 0;
	set_value(s, 0);
	return VK_SUCCESS;
}

static bool
satisfied(const struct wgvk_sync *s, const struct vk_sync_wait *w)
{
	return (w->sync->flags & VK_SYNC_IS_TIMELINE) ? s->value >= w->wait_value : s->value != 0;
}

/* The serial whose completion satisfies the wait, or 0 if no submitted signal can. */
static uint64_t
serial_for(const struct wgvk_sync *s, const struct vk_sync_wait *w)
{
	uint64_t best = 0;
	for (unsigned i = 0; i < s->n_pending; i++) {
		bool ok = (w->sync->flags & VK_SYNC_IS_TIMELINE) ? s->pending[i].value >= w->wait_value : true;
		if (ok && (!best || s->pending[i].serial < best))
			best = s->pending[i].serial;
	}
	return best;
}

static VkResult
sync_wait_many(struct vk_device *device, uint32_t wait_count, const struct vk_sync_wait *waits,
	       enum vk_sync_wait_flags wait_flags, uint64_t abs_timeout_ns)
{
	struct wgvk_device *dev = container_of(device, struct wgvk_device, vk);
	bool any = wait_flags & VK_SYNC_WAIT_ANY;
	for (;;) {
		wgvk_queue_poll(dev, 0, 0);
		uint32_t done = 0;
		uint64_t next = 0;	/* the serial to wait for */
		bool cpu_only = false;	/* a wait no submitted signal can satisfy */
		for (uint32_t i = 0; i < wait_count; i++) {
			struct wgvk_sync *s = container_of(waits[i].sync, struct wgvk_sync, base);
			wgvk_sync_complete(dev, s);
			if (satisfied(s, &waits[i])) {
				done++;
				continue;
			}
			uint64_t sn = serial_for(s, &waits[i]);
			if ((wait_flags & VK_SYNC_WAIT_PENDING) && sn) {
				done++;	/* "pending" = submitted */
				continue;
			}
			if (!sn)
				cpu_only = true;
			else if (!next || (any ? sn < next : sn > next))
				next = sn;
		}
		if (done == wait_count || (any && done))
			return VK_SUCCESS;
		if (abs_timeout_ns == 0)
			return VK_TIMEOUT;
		if (next) {
			VkResult r = wgvk_queue_poll(dev, next, abs_timeout_ns);
			if (r != VK_SUCCESS)
				return r;
			continue;
		}
		(void)cpu_only;
		/* only the CPU (another thread) can still signal it */
#ifdef __EMSCRIPTEN__
		return VK_TIMEOUT;
#else
		if (os_time_get_nano() >= abs_timeout_ns)
			return VK_TIMEOUT;
		os_time_sleep(100);
#endif
	}
}

/* Sync files (T7). Mesa's venus over vtest offers a swapchain only when the host's semaphores import sync
 * files, and its WSI then signals them with "already signalled" ones (fd -1: vkr imports those; the runtime
 * signals for us). There are no fences between processes here: a real sync file is waited for (natively),
 * and an exported one is -1 once the sync's work is done (a signalled sync file, which Vulkan allows). */
static VkResult
sync_import_sync_file(struct vk_device *device, struct vk_sync *sync, int sync_file)
{
#ifndef __EMSCRIPTEN__
	struct pollfd p = { .fd = sync_file, .events = POLLIN };
	while (poll(&p, 1, -1) < 0 && errno == EINTR)
		;
#endif
	return sync_signal(device, sync, 0);
}

static VkResult
sync_export_sync_file(struct vk_device *device, struct vk_sync *sync, int *sync_file)
{
	const struct wgvk_sync *ws = container_of(sync, struct wgvk_sync, base);
	const struct vk_sync_wait w = { .sync = sync, .stage_mask = ~(VkPipelineStageFlags2)0, .wait_value = 0 };
	/* (nothing submitted will signal it: nothing to wait for) */
	VkResult r = ws->n_pending ? sync_wait_many(device, 1, &w, VK_SYNC_WAIT_COMPLETE, UINT64_MAX) : VK_SUCCESS;
	if (r != VK_SUCCESS)
		return r;
	*sync_file = -1;
	return VK_SUCCESS;
}

const struct vk_sync_type wgvk_sync_type = {
	.size = sizeof(struct wgvk_sync),
	.features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_TIMELINE | VK_SYNC_FEATURE_GPU_WAIT |
		    VK_SYNC_FEATURE_GPU_MULTI_WAIT | VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_RESET |
		    VK_SYNC_FEATURE_CPU_SIGNAL | VK_SYNC_FEATURE_WAIT_ANY | VK_SYNC_FEATURE_WAIT_PENDING,
	.init = sync_init,
	.finish = sync_finish,
	.signal = sync_signal,
	.get_value = sync_get_value,
	.reset = sync_reset,
	.wait_many = sync_wait_many,
	.import_sync_file = sync_import_sync_file,
	.export_sync_file = sync_export_sync_file,
};
