/*
 * vrend-replay: replay a vfront capture (tools/vf_capture.h) into virglrenderer's vrend (the reference: what
 * virgl_test_server would have drawn, here on the VM's llvmpipe through EGL surfaceless) and write every
 * TRANSFER_GET as an image, named and listed as vfront-replay does, so the two can be compared image by image
 * (tools/imgcmp.py).
 *
 *   vrend-replay CAPTURE [--dump DIR] [--every N] [--min-size N]
 *
 * Built against the system's libvirglrenderer (Debian trixie's 1.1.0 in the chroot) with virglrenderer's
 * public header; it needs no Mesa: only the formats windows use are converted to images.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "virglrenderer.h"
#include "vf_capture.h"
#include "virgl_hw.h"

struct res {
	uint32_t id, ctx;
	uint32_t target, format, width, height;
	uint8_t *mem;
	uint64_t size;
	struct iovec iov;
	struct res *next;
};

static struct res *resources;
static const char *dump_dir;
static unsigned min_size = 32, every = 1, dumped, readbacks;
static uint32_t last_fence;

static void write_fence(void *cookie, uint32_t fence)
{
	(void)cookie;
	last_fence = fence;
}

static struct virgl_renderer_callbacks cbs = { .version = 1, .write_fence = write_fence };

static struct res *res_find(uint32_t id)
{
	for (struct res *r = resources; r; r = r->next)
		if (r->id == id)
			return r;
	return NULL;
}

/* bytes per pixel and how to get R, G, B, or 0 when this tool does not convert the format */
static unsigned fmt_info(uint32_t f, int *ri, int *gi, int *bi)
{
	switch (f) {
	case VIRGL_FORMAT_B8G8R8A8_UNORM:
	case VIRGL_FORMAT_B8G8R8X8_UNORM:
	case VIRGL_FORMAT_B8G8R8A8_SRGB:
	case VIRGL_FORMAT_B8G8R8X8_SRGB:
		*ri = 2, *gi = 1, *bi = 0;
		return 4;
	case VIRGL_FORMAT_R8G8B8A8_UNORM:
	case VIRGL_FORMAT_R8G8B8X8_UNORM:
	case VIRGL_FORMAT_R8G8B8A8_SRGB:
	case VIRGL_FORMAT_R8G8B8X8_SRGB:
		*ri = 0, *gi = 1, *bi = 2;
		return 4;
	case VIRGL_FORMAT_A8R8G8B8_UNORM:
	case VIRGL_FORMAT_X8R8G8B8_UNORM:
		*ri = 1, *gi = 2, *bi = 3;
		return 4;
	case VIRGL_FORMAT_A8B8G8R8_UNORM:
	case VIRGL_FORMAT_X8B8G8R8_UNORM:
		*ri = 3, *gi = 2, *bi = 1;
		return 4;
	}
	return 0;
}

struct get {
	uint32_t ctx, res, level, x, y, w, h, d, offset;
};

static void dump(const struct get *g)
{
	struct res *r = res_find(g->res);
	int ri, gi, bi;
	if (!r || !dump_dir || g->level || g->d != 1 || g->w < min_size || g->h < min_size)
		return;
	unsigned bpp = fmt_info(r->format, &ri, &gi, &bi);
	/* numbered by readback, as vfront-replay does, so the images pair up */
	dumped = readbacks++;
	if (!bpp || dumped % every)
		return;
	char path[512];
	snprintf(path, sizeof(path), "%s/get-%05u.ppm", dump_dir, dumped);
	FILE *o = fopen(path, "wb");
	if (!o)
		return;
	fprintf(o, "P6\n%u %u\n255\n", g->w, g->h);
	unsigned stride = r->width * bpp;
	uint8_t *rgb = malloc(g->w * 3);
	for (unsigned y = 0; y < g->h; y++) {
		uint64_t off = g->offset + (uint64_t)y * stride;
		if (off + (uint64_t)g->w * bpp > r->size)
			break;
		const uint8_t *s = r->mem + off;
		for (unsigned x = 0; x < g->w; x++) {
			rgb[x * 3 + 0] = s[x * bpp + ri];
			rgb[x * 3 + 1] = s[x * bpp + gi];
			rgb[x * 3 + 2] = s[x * bpp + bi];
		}
		fwrite(rgb, 3, g->w, o);
	}
	free(rgb);
	fclose(o);
	snprintf(path, sizeof(path), "%s/index.txt", dump_dir);
	o = fopen(path, "a");
	if (o) {
		fprintf(o, "get-%05u.ppm ctx %u res %u format %u %ux%u at %u,%u\n", dumped, g->ctx, g->res, r->format,
			g->w, g->h, g->x, g->y);
		fclose(o);
	}
}

/* The guest was given vfront's caps, not vrend's: where the two differ in meaning, the stream is changed so
 * vrend draws what the guest asked for of a host with those caps. Without a lower-left gl_FragCoord origin in
 * the caps, the guest transforms gl_FragCoord itself and the program's FS_COORD_ORIGIN property is stale
 * (hosts without lower-left ignore it); vrend would honour it and flip twice, so it is made UPPER_LEFT. */
static bool caps_lower_left = true;

static void adapt_submit(uint32_t *dw, uint32_t ndw)
{
	if (caps_lower_left)
		return;
	for (uint32_t off = 0; off < ndw;) {
		uint32_t len = dw[off] >> 16, cmd = dw[off] & 0xff, obj = (dw[off] >> 8) & 0xff;
		if (cmd == 1 /* CREATE_OBJECT */ && obj == 4 /* SHADER */ && off + 1 + len <= ndw) {
			char *t = (char *)&dw[off + 1];
			size_t n = (size_t)len * 4;
			const char *k = "FS_COORD_ORIGIN LOWER_LEFT";
			size_t kl = strlen(k);
			for (size_t i = 0; i + kl <= n; i++)
				if (!memcmp(t + i, k, kl))
					memcpy(t + i + 16, "UPPER", 5);
		}
		off += len + 1;
	}
}

static void wait_fence(uint32_t serial)
{
	for (int i = 0; i < 100000 && (int32_t)(last_fence - serial) < 0; i++)
		virgl_renderer_poll();
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump") && i + 1 < argc)
			dump_dir = argv[++i];
		else if (!strcmp(argv[i], "--min-size") && i + 1 < argc)
			min_size = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--every") && i + 1 < argc)
			every = atoi(argv[++i]);
		else if (!path)
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: vrend-replay CAPTURE [--dump DIR] [--min-size N]\n");
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
	static int cookie;
	if (virgl_renderer_init(&cookie, VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_USE_SURFACELESS, &cbs)) {
		fprintf(stderr, "virglrenderer did not start\n");
		return 1;
	}
	{
		uint32_t max_ver = 0, max_size = 0;
		virgl_renderer_get_cap_set(2, &max_ver, &max_size);
		union virgl_caps *caps = calloc(1, max_size > sizeof(*caps) ? max_size : sizeof(*caps));
		virgl_renderer_fill_caps(2, max_ver, caps);
		printf("vrend renderer: %s\n", caps->v2.renderer);
		free(caps);
	}
	unsigned records = 0, submits = 0, failed = 0;
	struct vfcap_hdr h;
	uint8_t *p = NULL;
	size_t cap_p = 0;
	struct get pending[64];
	unsigned npending = 0;
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
			p[h.bytes] = 0;
			virgl_renderer_context_create(h.ctx, strlen((char *)p), (char *)p);
			break;
		case VFCAP_CTX_DESTROY:
			virgl_renderer_context_destroy(h.ctx);
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
			struct virgl_renderer_resource_create_args a = {
				.handle = w[0], .target = w[1], .format = w[2], .bind = w[3], .width = w[4],
				.height = w[5], .depth = w[6], .array_size = w[7], .last_level = w[8],
				.nr_samples = w[9], .flags = w[10],
			};
			if (virgl_renderer_resource_create(&a, NULL, 0)) {
				failed++;
				break;
			}
			if (r->mem) {
				r->iov.iov_base = r->mem;
				r->iov.iov_len = r->size;
				virgl_renderer_resource_attach_iov(r->id, &r->iov, 1);
			}
			virgl_renderer_ctx_attach_resource(h.ctx, r->id);
			break;
		}
		case VFCAP_RES_UNREF:
			virgl_renderer_ctx_detach_resource(h.ctx, w[0]);
			virgl_renderer_resource_unref(w[0]);
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
		case VFCAP_CAPS: {
			union virgl_caps *c = (union virgl_caps *)p;
			if (h.bytes >= sizeof(c->v1))
				caps_lower_left = c->v1.bset.fragment_coord_conventions;
			break;
		}
		case VFCAP_SUBMIT:
			adapt_submit(w, h.bytes / 4);
			if (virgl_renderer_submit_cmd(w, h.ctx, h.bytes / 4))
				failed++;
			submits++;
			break;
		case VFCAP_TRANSFER_PUT:
		case VFCAP_TRANSFER_GET: {
			struct virgl_box box = { w[2], w[3], w[4], w[5], w[6], w[7] };
			if (h.op == VFCAP_TRANSFER_PUT) {
				virgl_renderer_transfer_write_iov(w[0], h.ctx, w[1], 0, 0, &box, w[8], NULL, 0);
			} else {
				virgl_renderer_transfer_read_iov(w[0], h.ctx, w[1], 0, 0, &box, w[8], NULL, 0);
				if (npending < 64)
					pending[npending++] = (struct get){ h.ctx, w[0], w[1], w[2], w[3], w[5], w[6], w[7], w[8] };
			}
			break;
		}
		case VFCAP_DATA: {
			struct res *r = res_find(w[0]);
			if (r && (uint64_t)w[1] + w[2] <= r->size)
				memcpy(r->mem + w[1], p + 12, w[2]);
			break;
		}
		case VFCAP_FLUSH:
			virgl_renderer_create_fence(h.serial, h.ctx);
			break;
		case VFCAP_WAIT:
			wait_fence(h.serial);
			for (unsigned i = 0; i < npending; i++)
				if (pending[i].ctx == h.ctx)
					dump(&pending[i]);
			{
				unsigned k = 0;
				for (unsigned i = 0; i < npending; i++)
					if (pending[i].ctx != h.ctx)
						pending[k++] = pending[i];
				npending = k;
			}
			break;
		}
	}
	printf("replayed %u records into vrend: %u submits (%u failed), %u readbacks\n", records, submits, failed, readbacks);
	return failed ? 1 : 0;
}
