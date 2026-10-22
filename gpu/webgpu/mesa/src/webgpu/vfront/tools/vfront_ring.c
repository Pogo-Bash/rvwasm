/*
 * vfront-ring: a capture (vf_capture.h) played as gpuserver would, through the ring (gpu/PROTOCOL.md, ring
 * v2) into rvgpu's ring consumer (rvgpu.c on rvgpu_native.c's guest memory) and vfront's ring glue
 * (vf_rvgpu.c): the page's path natively, minus WebGPU. Backings live in the "guest memory" at addresses
 * given in RES_CREATE, as gpuserver gives them; a TRANSFER_GET's bytes are checked against the live run's
 * RESULT records once ctx_done has passed the record, as the guest would wait for them.
 *
 *   vfront-ring CAPTURE        (VFRONT_GIPA_LIB: zink on that driver without a loader, as in the page)
 *
 * Prints "ok ring-vfront: ..." or "FAIL ring-vfront: ...".
 *
 * SPDX-License-Identifier: MIT
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "rvgpu.h"
#include "vf_capture.h"
#include "vfront.h"

int vfront_rvgpu_start_with(void *gipa, uint32_t flags);

#define MEM_SIZE (2ull << 30)	/* the guest memory */
#define RING 4096u
#define DATA_OFF 4096u
#define DATA_SIZE (32u << 20)	/* a power of two, as gpuserver's */
#define CAPS (RING + DATA_OFF + DATA_SIZE)
#define HEAP (CAPS + 65536)	/* backings from here */

static uint8_t *mem;
static uint32_t head, serial, last_tracked;	/* (the newest SUBMIT/PUT/GET: what done must reach) */
static uint64_t heap = HEAP;

struct res {
	uint32_t id, ctx;
	uint64_t addr, size;
	struct res *next;
};
static struct res *resources;

static struct res *res_find(uint32_t id)
{
	for (struct res *r = resources; r; r = r->next)
		if (r->id == id)
			return r;
	return NULL;
}

static uint32_t r32(uint64_t a)
{
	uint32_t v;
	memcpy(&v, mem + a, 4);
	return v;
}

static void w32(uint64_t a, uint32_t v)
{
	memcpy(mem + a, &v, 4);
}

/* A record into the ring (a PAD to the end first when it would not fit before the wrap), then drained. */
static uint32_t put(uint32_t op, uint32_t ctx, const void *payload, uint32_t bytes)
{
	uint32_t need = 16 + ((bytes + 15) & ~15u), pos = head % DATA_SIZE;
	if (need > DATA_SIZE) {
		fprintf(stderr, "vfront-ring: a record of %u bytes does not fit the ring\n", bytes);
		exit(1);
	}
	if (DATA_SIZE - pos < need) {
		uint32_t rest = DATA_SIZE - pos;
		uint32_t pad[4] = { RVGPU_OP_PAD, 0, 0, rest - 16 };
		memcpy(mem + RING + DATA_OFF + pos, pad, 16);
		head += rest;
		pos = 0;
	}
	uint32_t s = ++serial;
	uint32_t h[4] = { op, ctx, s, bytes };
	memcpy(mem + RING + DATA_OFF + pos, h, 16);
	memcpy(mem + RING + DATA_OFF + pos + 16, payload, bytes);
	head += need;
	if (op == RVGPU_OP_SUBMIT || op == RVGPU_OP_TRANSFER_PUT || op == RVGPU_OP_TRANSFER_GET)
		last_tracked = s;
	w32(RING + RVGPU_RING_HEAD, head);
	rvgpu_ring_drain();
	return s;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: vfront-ring CAPTURE\n");
		return 2;
	}
	FILE *in = fopen(argv[1], "rb");
	char magic[8];
	if (!in || fread(magic, 1, 8, in) != 8 || memcmp(magic, VFCAP_MAGIC, 8)) {
		printf("FAIL ring-vfront: %s is not a capture\n", argv[1]);
		return 1;
	}
	mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	rvgpu_guest_set_host(mem, MEM_SIZE);
	w32(RING + RVGPU_RING_MAGIC, RVGPU_RING_MAGIC_VALUE);
	w32(RING + RVGPU_RING_VERSION, 2);
	w32(RING + RVGPU_RING_DATA_SIZE, DATA_SIZE);
	w32(RING + RVGPU_RING_DATA_OFF, DATA_OFF);
	if (rvgpu_ring_attach(RING, DATA_OFF + DATA_SIZE, CAPS, 4096)) {
		printf("FAIL ring-vfront: attach\n");
		return 1;
	}
	void *gipa = NULL;
	const char *lib = getenv("VFRONT_GIPA_LIB");
	if (lib) {
		void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
		gipa = h ? dlsym(h, "vk_icdGetInstanceProcAddr") : NULL;
		if (!gipa) {
			printf("FAIL ring-vfront: %s has no vk_icdGetInstanceProcAddr\n", lib);
			return 1;
		}
	}
	/* (one thread, as in the page) */
	if (vfront_rvgpu_start_with(gipa, VF_INIT_SINGLE_THREAD) || r32(RING + RVGPU_RING_EXEC_STATE) != 1) {
		printf("FAIL ring-vfront: vfront did not start (exec_state %u)\n", r32(RING + RVGPU_RING_EXEC_STATE));
		return 1;
	}
	uint32_t caps_len = r32(RING + RVGPU_RING_CAPS_LEN);

	struct vfcap_hdr h;
	uint8_t *p = NULL;
	size_t cap_p = 0;
	unsigned records = 0, results = 0, mismatches = 0, late = 0, gets = 0;
	uint32_t last_get[256] = { 0 };
	while (fread(&h, sizeof(h), 1, in) == 1) {
		if (h.bytes + 4 > cap_p) {
			cap_p = h.bytes + 4;
			p = realloc(p, cap_p);
		}
		if (h.bytes && fread(p, 1, h.bytes, in) != h.bytes)
			break;
		uint32_t *w = (uint32_t *)p;
		records++;
		switch (h.op) {
		case VFCAP_CTX_CREATE:
		case VFCAP_CTX_DESTROY:
		case VFCAP_SUBMIT:
		case VFCAP_TRANSFER_PUT:
			put(h.op, h.ctx, p, h.bytes);
			break;
		case VFCAP_TRANSFER_GET:
			last_get[h.ctx & 255] = put(h.op, h.ctx, p, h.bytes);
			gets++;
			break;
		case VFCAP_RES_CREATE: {
			struct res *r = calloc(1, sizeof(*r));
			r->id = w[0];
			r->ctx = h.ctx;
			r->size = w[11];
			if (r->size) {
				r->addr = heap;
				heap = (heap + r->size + 4095) & ~4095ull;
				if (heap > MEM_SIZE) {
					printf("FAIL ring-vfront: backings past %llu MB\n", (unsigned long long)(MEM_SIZE >> 20));
					return 1;
				}
			}
			r->next = resources;
			resources = r;
			w[12] = (uint32_t)r->addr;
			w[13] = (uint32_t)(r->addr >> 32);
			put(h.op, h.ctx, p, h.bytes);
			break;
		}
		case VFCAP_RES_UNREF:
			put(h.op, h.ctx, p, h.bytes);
			break;
		case VFCAP_DATA: {
			/* what the guest had in the backing when the next record was decoded */
			struct res *r = res_find(w[0]);
			if (r && (uint64_t)w[1] + w[2] <= r->size)
				memcpy(mem + r->addr + w[1], p + 12, w[2]);
			break;
		}
		case VFCAP_RESULT: {
			struct res *r = res_find(w[0]);
			if (!r)
				break;
			/* the guest reads a readback only once ctx_done has passed its TRANSFER_GET */
			uint32_t done = r32(RING + RVGPU_RING_CTX_DONE + 4 * (r->ctx & 255));
			if ((int32_t)(done - last_get[r->ctx & 255]) < 0)
				late++;
			results++;
			uint64_t want = w[3] | (uint64_t)w[4] << 32;
			if ((uint64_t)w[1] + w[2] > r->size || vfcap_hash(mem + r->addr + w[1], w[2]) != want)
				mismatches++;
			break;
		}
		default:	/* FLUSH, WAIT, CAPS: the ring has no such records */
			break;
		}
	}
	uint32_t tail = r32(RING + RVGPU_RING_TAIL), done = r32(RING + RVGPU_RING_DONE);
	struct vf_stats st;
	vf_get_stats(&st);
	bool ok = !mismatches && !late && tail == head && done == last_tracked;
	printf("%s ring-vfront %s: %u records, %u ring records (%llu draws), %u readbacks: %u of %u equal to the "
	       "live run's, %u read before ctx_done passed them; tail %s head, done %u of %u; %u bytes of caps\n",
	       ok ? "ok" : "FAIL", strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1], records, serial,
	       (unsigned long long)st.draws, gets, results - mismatches, results, late,
	       tail == head ? "=" : "!=", done, last_tracked, caps_len);
	return ok ? 0 : 1;
}
