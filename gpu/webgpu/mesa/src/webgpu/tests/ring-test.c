/*
 * rvgpu's ring consumer natively (rvgpu.c on rvgpu_native.c): a ring v2 laid
 * out as gpuserver does (gpu/PROTOCOL.md) in a host block, records written
 * across the wrap with a PAD, dispatch to a registered op, completion in
 * order (done) and per context (ctx_done), the tail. Prints "ok ring: ..." or
 * "FAIL ring: ...".
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rvgpu.h"

#define RING 4096u		/* guest address of the ring header */
#define DATA_OFF 2048u
#define DATA_SIZE 4096u
#define CAPS (RING + DATA_OFF + DATA_SIZE)

static uint8_t mem[64 * 1024];
static uint32_t head;
static unsigned submits, sum;
static uint32_t held_ctx, held_serial;	/* one completion held back to check ordering */

static void
w32(uint32_t addr, uint32_t v)
{
	memcpy(mem + addr, &v, 4);
}

static uint32_t
r32(uint32_t addr)
{
	uint32_t v;
	memcpy(&v, mem + addr, 4);
	return v;
}

static void
put(uint32_t op, uint32_t ctx, uint32_t serial, const void *payload, uint32_t bytes)
{
	uint32_t need = 16 + ((bytes + 15) & ~15u), pos = head % DATA_SIZE;
	if (DATA_SIZE - pos < need) {	/* PAD to the end */
		uint32_t rest = DATA_SIZE - pos;
		uint32_t pad[4] = { RVGPU_OP_PAD, 0, 0, rest - 16 };
		memcpy(mem + RING + DATA_OFF + pos, pad, 16);
		head += rest;
		pos = 0;
	}
	uint32_t h[4] = { op, ctx, serial, bytes };
	memcpy(mem + RING + DATA_OFF + pos, h, 16);
	memcpy(mem + RING + DATA_OFF + pos + 16, payload, bytes);
	head += need;
	w32(RING + RVGPU_RING_HEAD, head);
}

static void
on_submit(void *front, const struct rvgpu_record *rec, const void *payload)
{
	const uint32_t *d = payload;
	submits++;
	for (uint32_t i = 0; i < rec->bytes / 4; i++)
		sum += d[i];
	if (rec->serial == 2) {	/* finish this one later */
		held_ctx = rec->ctx;
		held_serial = rec->serial;
		return;
	}
	rvgpu_complete(rec->ctx, rec->serial);
}

int
main(void)
{
	rvgpu_guest_set_host(mem, sizeof(mem));
	w32(RING + RVGPU_RING_MAGIC, RVGPU_RING_MAGIC_VALUE);
	w32(RING + RVGPU_RING_VERSION, 2);
	w32(RING + RVGPU_RING_DATA_SIZE, DATA_SIZE);
	w32(RING + RVGPU_RING_DATA_OFF, DATA_OFF);
	if (rvgpu_ring_attach(RING, DATA_OFF + DATA_SIZE, CAPS, 4096)) {
		printf("FAIL ring: attach\n");
		return 1;
	}
	rvgpu_ring_register(RVGPU_OP_SUBMIT, on_submit, NULL);
	uint32_t payload[200];
	for (int i = 0; i < 200; i++)
		payload[i] = i;
	char name[] = "test";
	put(RVGPU_OP_CTX_CREATE, 1, 0, name, sizeof(name));
	uint32_t serial = 0;
	unsigned expect_sum = 0;
	/* 12 submits of 800 bytes: wraps the 4 KiB data area twice */
	for (int k = 0; k < 12; k++) {
		put(RVGPU_OP_SUBMIT, 1 + (k & 1), ++serial, payload, 800);
		for (int i = 0; i < 200; i++)
			expect_sum += i;
		if (k == 3 || k == 7 || k == 11)
			rvgpu_ring_drain();
	}
	int fails = 0;
	if (submits != 12 || sum != expect_sum) {
		printf("FAIL ring: %u submits (want 12), sum %u (want %u)\n", submits, sum, expect_sum);
		fails++;
	}
	if (r32(RING + RVGPU_RING_TAIL) != head) {
		printf("FAIL ring: tail %u, head %u\n", r32(RING + RVGPU_RING_TAIL), head);
		fails++;
	}
	/* serial 2 (ctx 2's first) held: done stops at 1, ctx 1 is at 11, ctx 2 has nothing in order yet */
	uint32_t done = r32(RING + RVGPU_RING_DONE), d1 = r32(RING + RVGPU_RING_CTX_DONE + 4), d2 = r32(RING + RVGPU_RING_CTX_DONE + 8);
	if (done != 1 || d1 != 11 || d2 != 0) {
		printf("FAIL ring: before the held completion done=%u ctx1=%u ctx2=%u (want 1 11 0)\n", done, d1, d2);
		fails++;
	}
	rvgpu_complete(held_ctx, held_serial);
	done = r32(RING + RVGPU_RING_DONE);
	d2 = r32(RING + RVGPU_RING_CTX_DONE + 8);
	if (done != 12 || d2 != 12) {
		printf("FAIL ring: after it done=%u ctx2=%u (want 12 12)\n", done, d2);
		fails++;
	}
	if (!fails)
		printf("ok ring: 13 records over 2 wraps, dispatch, in-order done and ctx_done with one completion held back, tail = head (%u)\n", head);
	return fails ? 1 : 0;
}
