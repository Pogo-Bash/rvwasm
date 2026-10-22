/*
 * vfront-replay: replay a capture (tools/vf_capture.h) into vfront and write what every TRANSFER_GET read
 * back as an image, to compare with the same capture replayed into virglrenderer (vrend-replay) or with an
 * earlier run.
 *
 *   vfront-replay CAPTURE [--dump DIR] [--every N] [--min-size N] [--times]
 *
 * Images: DIR/get-NNNNN.ppm for every readback of a 2D colour box at least N pixels on each side (default
 * 32), in order (with --every N only every Nth); DIR/index.txt lists them with resource, format and size. The RESULT records of the capture
 * (what vfront wrote during the live run) are compared with what the replay writes, which tells whether the
 * replay is faithful.
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "vfront.h"
#include "vf_capture.h"
#include "virgl_hw.h"

#include "util/format/u_format.h"

enum pipe_format vf_format(uint32_t virgl_format);

struct res {
	uint32_t id, ctx;
	uint32_t target, format, width, height;
	uint8_t *mem;
	uint64_t size;
	struct res *next;
};

static struct res *resources;
static const char *dump_dir;
static unsigned min_size = 32, every = 1, dumped, readbacks, results, result_mismatch;

static struct res *res_find(uint32_t id)
{
	for (struct res *r = resources; r; r = r->next)
		if (r->id == id)
			return r;
	return NULL;
}

struct get {
	uint32_t ctx, res, level;
	struct vf_box box;
	uint32_t offset;
	bool pending;
};
static struct get gets[64];
static unsigned num_gets, readbacks_seen;

static void dump(struct get *g)
{
	struct res *r = res_find(g->res);
	if (!r || !dump_dir || g->level || g->box.d != 1 || g->box.w < min_size || g->box.h < min_size)
		return;
	enum pipe_format f = vf_format(r->format);
	const struct util_format_description *d = util_format_description(f);
	if (!d || d->colorspace == UTIL_FORMAT_COLORSPACE_ZS || util_format_is_compressed(f))
		return;
	/* (numbered by readback, so two replays' images pair up whatever --every says) */
	dumped = readbacks++;
	if (dumped % every)
		return;
	unsigned stride = util_format_get_stride(f, r->width);
	char path[512];
	snprintf(path, sizeof(path), "%s/get-%05u.ppm", dump_dir, dumped);
	FILE *o = fopen(path, "wb");
	if (!o)
		return;
	fprintf(o, "P6\n%u %u\n255\n", g->box.w, g->box.h);
	uint8_t *rgba = malloc(g->box.w * 4), *rgb = malloc(g->box.w * 3);
	for (unsigned y = 0; y < g->box.h; y++) {
		uint64_t off = g->offset + (uint64_t)y * stride;
		if (off + util_format_get_stride(f, g->box.w) > r->size)
			break;
		util_format_unpack_rgba_8unorm_rect(f, rgba, 0, r->mem + off, 0, g->box.w, 1);
		for (unsigned x = 0; x < g->box.w; x++)
			memcpy(rgb + x * 3, rgba + x * 4, 3);
		fwrite(rgb, 3, g->box.w, o);
	}
	free(rgba);
	free(rgb);
	fclose(o);
	snprintf(path, sizeof(path), "%s/index.txt", dump_dir);
	o = fopen(path, "a");
	if (o) {
		fprintf(o, "get-%05u.ppm ctx %u res %u %s %ux%u at %u,%u\n", dumped, g->ctx, g->res,
			util_format_short_name(f), g->box.w, g->box.h, g->box.x, g->box.y);
		fclose(o);
	}
}

/* Readbacks of ctx are in their backings once its work is done: write them out. */
static void complete(uint32_t ctx)
{
	vf_ctx_poll(ctx, UINT64_MAX);
	for (unsigned i = 0; i < num_gets; i++) {
		if (gets[i].pending && gets[i].ctx == ctx) {
			dump(&gets[i]);
			gets[i].pending = false;
		}
	}
	unsigned k = 0;
	for (unsigned i = 0; i < num_gets; i++)
		if (gets[i].pending)
			gets[k++] = gets[i];
	num_gets = k;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* In the page (vf_rvgpu.c) this runs as vfront_replay_main, with zink on wgvk and no driver threads. */
#ifndef VF_REPLAY_MAIN
#define VF_REPLAY_MAIN main
#endif
#ifdef VF_REPLAY_PAGE
#include "wgvk.h"
int VF_REPLAY_MAIN(int argc, char **argv);
#endif

int VF_REPLAY_MAIN(int argc, char **argv)
{
	const char *path = NULL;
	bool times = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump") && i + 1 < argc)
			dump_dir = argv[++i];
		else if (!strcmp(argv[i], "--min-size") && i + 1 < argc)
			min_size = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--every") && i + 1 < argc)
			every = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--times"))
			times = true;
		else if (!path)
			path = argv[i];
		else {
			fprintf(stderr, "usage: vfront-replay CAPTURE [--dump DIR] [--every N] [--min-size N] [--times]\n");
			return 2;
		}
	}
	if (!path) {
		fprintf(stderr, "usage: vfront-replay CAPTURE [--dump DIR] [--every N] [--min-size N] [--times]\n");
		return 2;
	}
	FILE *in = fopen(path, "rb");
	char magic[8];
	if (!in || fread(magic, 1, 8, in) != 8 || memcmp(magic, VFCAP_MAGIC, 8)) {
		fprintf(stderr, "%s: not a vfront capture\n", path);
		return 1;
	}
	if (dump_dir) {
		mkdir(dump_dir, 0755);
		char p[512];
		snprintf(p, sizeof(p), "%s/index.txt", dump_dir);
		remove(p);
	}
#ifdef VF_REPLAY_PAGE
	struct vf_init_args init = {
		.vk_get_instance_proc_addr = (void *)wgvk_GetInstanceProcAddr,
		.flags = VF_INIT_SINGLE_THREAD,
	};
	if (vf_init(&init)) {
#else
	/* VFRONT_GIPA_LIB: zink on that Vulkan driver's vk_icdGetInstanceProcAddr with no loader, as in the
	 * page (wgvk natively: libvulkan_webgpu.so) */
	struct vf_init_args init = { 0 };
	const char *lib = getenv("VFRONT_GIPA_LIB");
	if (lib) {
		void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
		init.vk_get_instance_proc_addr = h ? dlsym(h, "vk_icdGetInstanceProcAddr") : NULL;
		if (!init.vk_get_instance_proc_addr) {
			fprintf(stderr, "%s: no vk_icdGetInstanceProcAddr (%s)\n", lib, dlerror());
			return 1;
		}
	}
	if (vf_init(&init)) {
#endif
		fprintf(stderr, "vfront did not start\n");
		return 1;
	}
	uint64_t t_start = now_ns(), t_submit = 0;
	unsigned records = 0, submits = 0, failed = 0;
	struct vfcap_hdr h;
	uint8_t *p = NULL;
	size_t cap_p = 0;
	while (fread(&h, sizeof(h), 1, in) == 1) {
		if (h.bytes + 4 > cap_p) {
			cap_p = h.bytes + 4;
			p = realloc(p, cap_p);
		}
		if (h.bytes && fread(p, 1, h.bytes, in) != h.bytes)
			break;
		const uint32_t *w = (const uint32_t *)p;
		records++;
		switch (h.op) {
		case VFCAP_CTX_CREATE:
			p[h.bytes] = 0;
			vf_ctx_create(h.ctx, (const char *)p);
			break;
		case VFCAP_CTX_DESTROY:
			complete(h.ctx);
			vf_ctx_destroy(h.ctx);
			break;
		case VFCAP_RES_CREATE: {
			struct res *r = calloc(1, sizeof(*r));
			r->id = w[0];
			r->ctx = h.ctx;
			r->target = w[1];
			r->format = w[2];
			r->width = w[4];
			r->height = w[5];
			r->size = w[11];
			r->mem = r->size ? calloc(1, r->size) : NULL;
			r->next = resources;
			resources = r;
			struct vf_res_create_args a = { w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10] };
			if (vf_res_create(h.ctx, &a, (uintptr_t)r->mem, r->size))
				failed++;
			break;
		}
		case VFCAP_RES_UNREF: {
			complete(h.ctx);
			vf_res_unref(h.ctx, w[0]);
			for (struct res **pp = &resources; *pp; pp = &(*pp)->next) {
				if ((*pp)->id == w[0]) {
					struct res *r = *pp;
					*pp = r->next;
					free(r->mem);
					free(r);
					break;
				}
			}
			break;
		}
		case VFCAP_SUBMIT: {
			uint64_t t0 = now_ns();
			if (vf_submit(h.ctx, w, h.bytes / 4))
				failed++;
			t_submit += now_ns() - t0;
			submits++;
			break;
		}
		case VFCAP_TRANSFER_PUT:
		case VFCAP_TRANSFER_GET: {
			struct vf_box box = { w[2], w[3], w[4], w[5], w[6], w[7] };
			if (h.op == VFCAP_TRANSFER_PUT) {
				vf_transfer_put(h.ctx, w[0], w[1], &box, w[8]);
			} else {
				vf_transfer_get(h.ctx, w[0], w[1], &box, w[8]);
				if (getenv("VFRONT_REPLAY_TRACE")) {
					struct vf_stats st;
					vf_get_stats(&st);
					printf("readback %u of resource %u (%ux%u) after draw %llu\n", readbacks_seen, w[0],
					       w[5], w[6], (unsigned long long)st.draws);
				}
				readbacks_seen++;
				if (num_gets < 64)
					gets[num_gets++] = (struct get){ h.ctx, w[0], w[1], box, w[8], true };
			}
			break;
		}
		case VFCAP_DATA: {
			struct res *r = res_find(w[0]);
			if (r && (uint64_t)w[1] + w[2] <= r->size)
				memcpy(r->mem + w[1], p + 12, w[2]);
			break;
		}
		case VFCAP_RESULT: {
			struct res *r = res_find(w[0]);
			if (!r)
				break;
			complete(r->ctx);
			results++;
			uint64_t want = w[3] | (uint64_t)w[4] << 32;
			if ((uint64_t)w[1] + w[2] > r->size || vfcap_hash(r->mem + w[1], w[2]) != want)
				result_mismatch++;
			break;
		}
		case VFCAP_FLUSH:
			vf_ctx_flush(h.ctx, h.serial);
			break;
		case VFCAP_WAIT:
			complete(h.ctx);
			break;
		case VFCAP_CAPS:
			/* decisions that depend on what the guest was told follow the live run's caps */
			vf_set_guest_caps(p, h.bytes);
			break;
		}
	}
	for (unsigned c = 1; c < 256; c++)
		vf_ctx_poll(c, UINT64_MAX);
	uint64_t t_total = now_ns() - t_start;
	struct vf_stats st;
	vf_get_stats(&st);
	printf("replayed %u records: %u submits (%u failed), %llu commands, %llu draws, %llu shaders (%llu failed), "
	       "%u images; live-run results matched %u of %u\n", records, submits, failed,
	       (unsigned long long)st.commands, (unsigned long long)st.draws, (unsigned long long)st.shaders,
	       (unsigned long long)st.shader_failed, readbacks, results - result_mismatch, results);
	if (times)
		printf("time: total %.1f ms, in vf_submit %.1f ms (%.2f us per command, %.2f us per draw; shader "
		       "translation %.1f ms)\n", t_total / 1e6, t_submit / 1e6,
		       st.commands ? t_submit / 1e3 / st.commands : 0.0, st.draws ? (t_submit - st.shader_ns) / 1e3 / st.draws : 0.0,
		       st.shader_ns / 1e6);
	vf_fini();
	return failed ? 1 : 0;
}
