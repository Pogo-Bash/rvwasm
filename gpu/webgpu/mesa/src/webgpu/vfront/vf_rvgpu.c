/*
 * vfront in the page: gpuserver's ring (gpu/PROTOCOL.md, ring v2, read by rvgpu.c) into vfront, and vfront's
 * zink on wgvk. Started by vfront_rvgpu_start() once rvgpu has the ring and wgvk has the page's device.
 *
 * Completion (INTERFACES.md 3): records complete in order per context. Nothing the guest can observe needs
 * the GPU to have finished a SUBMIT, because every byte vfront reads from a backing is copied when the record
 * is decoded; only readbacks (TRANSFER_GET, and TRANSFER3D from the host) and query results write backings.
 * So a SUBMIT or TRANSFER_PUT completes as soon as it is flushed, and a record whose flush left readbacks or
 * query results completes when they are in the backings (vf_ctx_poll waits: under rvgpu_ring_drain, a JSPI
 * export, that suspends instead of blocking).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rvgpu.h"
#include "vfront.h"
#ifndef VF_RVGPU_NATIVE
#include "wgvk.h"
#endif

static void *mem_ptr(void *cookie, uint64_t addr, uint64_t size)
{
	(void)cookie;
	return rvgpu_guest_ptr(addr, size);	/* (NULL in the page: the machine's memory is not this module's) */
}

static void mem_read(void *cookie, uint64_t addr, void *dst, size_t size)
{
	(void)cookie;
	rvgpu_guest_read(dst, addr, size);
}

static void mem_write(void *cookie, uint64_t addr, const void *src, size_t size)
{
	(void)cookie;
	rvgpu_guest_write(addr, src, size);
}

static const struct vf_mem_ops mem = { NULL, mem_ptr, mem_read, mem_write };

static void flush_and_complete(uint32_t ctx, uint32_t serial)
{
	if (vf_ctx_flush(ctx, serial) > 0)
		vf_ctx_poll(ctx, UINT64_MAX);	/* readbacks or query results to wait for */
	else
		vf_ctx_poll(ctx, 0);
	rvgpu_complete(ctx, serial);
}

static void op_ctx_create(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	char name[64];
	size_t n = rec->bytes < sizeof(name) - 1 ? rec->bytes : sizeof(name) - 1;
	memcpy(name, payload, n);
	name[n] = 0;
	if (vf_ctx_create(rec->ctx, name))
		fprintf(stderr, "vfront: context %u (%s) could not be made\n", rec->ctx, name);
}

static void op_ctx_destroy(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	(void)payload;
	vf_ctx_destroy(rec->ctx);
}

static void op_res_create(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	const uint32_t *w = payload;
	if (rec->bytes < 14 * 4)
		return;
	struct vf_res_create_args a = { w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10] };
	vf_res_create(rec->ctx, &a, w[12] | (uint64_t)w[13] << 32, w[11]);
}

static void op_res_unref(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	if (rec->bytes >= 4)
		vf_res_unref(rec->ctx, *(const uint32_t *)payload);
}

static void op_submit(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	vf_submit(rec->ctx, payload, rec->bytes / 4);
	flush_and_complete(rec->ctx, rec->serial);
}

static void op_transfer(void *front, const struct rvgpu_record *rec, const void *payload)
{
	(void)front;
	const uint32_t *w = payload;
	if (rec->bytes < 10 * 4) {
		rvgpu_complete(rec->ctx, rec->serial);
		return;
	}
	struct vf_box box = { w[2], w[3], w[4], w[5], w[6], w[7] };
	if (rec->op == RVGPU_OP_TRANSFER_PUT) {
		vf_transfer_put(rec->ctx, w[0], w[1], &box, w[8]);
		rvgpu_complete(rec->ctx, rec->serial);
	} else {
		vf_transfer_get(rec->ctx, w[0], w[1], &box, w[8]);
		flush_and_complete(rec->ctx, rec->serial);
	}
}

/* vfront on the attached ring: zink on the Vulkan whose vkGetInstanceProcAddr is `gipa` (NULL: the system's
 * loader, natively), the ring ops registered, the virgl caps written and exec_state 1. Returns 0, or -1
 * (exec_state 3) when zink found no device. */
int vfront_rvgpu_start_with(void *gipa, uint32_t flags);

__attribute__((visibility("default"))) int
vfront_rvgpu_start_with(void *gipa, uint32_t flags)
{
	struct vf_init_args a = {
		.vk_get_instance_proc_addr = gipa,
		.mem = &mem,
		.flags = flags,
	};
	if (vf_init(&a)) {
		rvgpu_set_state(3, 1);
		return -1;
	}
	rvgpu_ring_register(RVGPU_OP_CTX_CREATE, op_ctx_create, NULL);
	rvgpu_ring_register(RVGPU_OP_CTX_DESTROY, op_ctx_destroy, NULL);
	rvgpu_ring_register(RVGPU_OP_RES_CREATE, op_res_create, NULL);
	rvgpu_ring_register(RVGPU_OP_RES_UNREF, op_res_unref, NULL);
	rvgpu_ring_register(RVGPU_OP_SUBMIT, op_submit, NULL);
	rvgpu_ring_register(RVGPU_OP_TRANSFER_PUT, op_transfer, NULL);
	rvgpu_ring_register(RVGPU_OP_TRANSFER_GET, op_transfer, NULL);
	static uint8_t caps[4096];
	uint32_t n = vf_caps(caps, sizeof(caps));
	rvgpu_set_caps(caps, n);
	rvgpu_set_state(1, 0);
	fprintf(stderr, "vfront: %s, %u bytes of virgl caps\n", vf_renderer_name(), n);
	return 0;
}

#ifndef VF_RVGPU_NATIVE
/* Called by the page Worker after rvgpu_ring_attach (and rvgpu_init, which gave wgvk the device): zink on
 * wgvk, everything on the Worker's thread. */
int vfront_rvgpu_start(void);
void vf_present_init(void);

__attribute__((visibility("default"))) int
vfront_rvgpu_start(void)
{
	int r = vfront_rvgpu_start_with((void *)wgvk_GetInstanceProcAddr, VF_INIT_SINGLE_THREAD);
	if (r == 0)
		vf_present_init();	/* (vf_present.c: windows that show a virgl resource, T7) */
	return r;
}

/* A capture (tools/vf_capture.h) replayed into vfront inside the page module: vfront on zink on wgvk on the
 * page's WebGPU device, without a machine or ring (tools/www/rvgpu-vfront.html; the Worker puts the capture
 * at /cap.vfcap in the module's file system first). Prints what vfront-replay prints. A JSPI export: the
 * replay waits for the GPU. */
int vfront_replay_main(int argc, char **argv);
int vfront_replay_page(void);

__attribute__((visibility("default"))) int
vfront_replay_page(void)
{
	char *argv[] = { "vfront-replay", "/cap.vfcap", "--times", NULL };
	int r = vfront_replay_main(3, argv);
	fflush(stdout);
	return r;
}
#endif
