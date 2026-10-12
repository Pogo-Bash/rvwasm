/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * The VA-API side: libva 2.x's driver interface (va_backend.h), for decoding (VAEntrypointVLD) only.
 *
 * Objects: surfaces are numbered from 1 and their number is the page's slot; configs, contexts, buffers and
 * images have ids of their own ranges (ID_*). One lock for the objects; the socket has its own. What waits
 * (vaSyncSurface, vaGetImage, room for a chunk) polls the status file without either.
 *
 * Environment: RVVDEC_DEBUG=1 says what happens on stderr; RVVDEC_DUMP=FILE writes every chunk there
 * ({u32 stream, u32 codec, u32 flags, u32 size} + bytes: gpu/vdec/test/chunks.py makes elementary streams of
 * it); RVVDEC_TIMEOUT=MS is how long a wait for the page may take (10000); RVVDEC_WAIT=S how long to wait
 * for the page's decoder to be there at the start (5); RVVDEC_SOCKET=PATH another gpuserver.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <va/va_drmcommon.h>

#include "vdec.h"

#define VENDOR "rvwasm WebCodecs (the page's VideoDecoder)"
#define MAX_PROFILES 16
#define CHUNK_AREA (8u << 20)
#define ALIGN(x, a) (((x) + (a) - 1) & ~((uint32_t)(a) - 1))

/* ---- the render node (Linux's include/uapi/drm/virtgpu_drm.h, drm.h): GPU resources for zero copy */
struct vg_resource_create {
	uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
	uint32_t bo_handle, res_handle, size, stride;
};
struct vg_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct vg_prime { uint32_t handle, flags; int32_t fd; };
struct vg_gem_close { uint32_t handle, pad; };
#define DRM_IOCTL_BASE 'd'
#define DRM_IOCTL_GEM_CLOSE _IOW(DRM_IOCTL_BASE, 0x09, struct vg_gem_close)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR(DRM_IOCTL_BASE, 0x2d, struct vg_prime)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE _IOWR(DRM_IOCTL_BASE, 0x40 + 0x04, struct vg_resource_create)
#define DRM_CLOEXEC 02000000
#define DRM_RDWR 02
#define PIPE_TEXTURE_2D 2
#define VIRGL_FORMAT_R8_UNORM 64
#define VIRGL_FORMAT_R8G8_UNORM 65
#define VIRGL_BIND_RENDER_TARGET (1 << 1)
#define VIRGL_BIND_SAMPLER_VIEW (1 << 3)
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_R8 FOURCC('R', '8', ' ', ' ')
#define DRM_FORMAT_GR88 FOURCC('G', 'R', '8', '8')
#define BACKING_SIZE 4096	/* the bytes never live in the machine: a stand-in (docs/video-decode.md, zero copy) */

void vlog(struct driver *d, const char *fmt, ...)
{
	va_list ap;

	(void)d;
	fprintf(stderr, "rvvdec: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

#define DRV(ctx) ((struct driver *)(ctx)->pDriverData)

static long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- small things */
int buf_put(struct buf *b, const void *p, size_t n)
{
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap : 65536;
		uint8_t *q;

		while (cap < b->n + n)
			cap *= 2;
		q = realloc(b->p, cap);
		if (!q)
			return -1;
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
	return 0;
}

int buf_byte(struct buf *b, uint8_t v)
{
	return buf_put(b, &v, 1);
}

void chunk_begin(struct chunks *o)
{
	if (o->n < MAX_CHUNKS) {
		o->c[o->n].at = o->b.n;
		o->c[o->n].slot = 0;
	}
}

void chunk_end(struct chunks *o, uint32_t flags)
{
	if (o->n < MAX_CHUNKS) {
		o->c[o->n].size = o->b.n - o->c[o->n].at;
		o->c[o->n].flags = flags;
		o->n++;
	}
}

/* The `index`-th buffer of `type` among the picture's (-1: the last). */
struct vbuf *find_buf(struct vbuf **bufs, int n, VABufferType type, int index)
{
	struct vbuf *last = NULL;

	for (int i = 0; i < n; i++) {
		if (bufs[i]->type == type) {
			if (index == 0)
				return bufs[i];
			index--;
			last = bufs[i];
		}
	}
	return index < 0 ? last : NULL;
}

static struct vsurface *surface_of(struct driver *d, VASurfaceID id)
{
	return id >= 1 && (int)id <= d->nsurfaces && d->surfaces[id - 1].used ? &d->surfaces[id - 1] : NULL;
}

uint32_t surface_slot(struct driver *d, VASurfaceID id)
{
	return surface_of(d, id) ? id : 0;
}

static struct vconfig *config_of(struct driver *d, VAConfigID id)
{
	uint32_t i = id - ID_CONFIG;

	return (id & ~ID_MASK) == ID_CONFIG && (int)i < d->nconfigs && d->configs[i].codec ? &d->configs[i] : NULL;
}

static struct vcontext *context_of(struct driver *d, VAContextID id)
{
	uint32_t i = id - ID_CONTEXT;

	return (id & ~ID_MASK) == ID_CONTEXT && (int)i < d->ncontexts && d->contexts[i].used ? &d->contexts[i] : NULL;
}

static struct vbuf *buffer_of(struct driver *d, VABufferID id)
{
	uint32_t i = id - ID_BUFFER;

	return (id & ~ID_MASK) == ID_BUFFER && (int)i < d->nbuffers ? d->buffers[i] : NULL;
}

static struct vimage *image_of(struct driver *d, VAImageID id)
{
	uint32_t i = id - ID_IMAGE;

	return (id & ~ID_MASK) == ID_IMAGE && (int)i < d->nimages && d->images[i].used ? &d->images[i] : NULL;
}

/* A free place in an array of `size`-byte elements whose first int says "used" (grown as needed). */
static int slot_in(void **array, int *count, size_t size)
{
	uint8_t *a = *array;
	int i;

	for (i = 0; i < *count; i++)
		if (!*(int *)(a + size * i))
			return i;
	a = realloc(a, size * (size_t)(*count ? *count * 2 : 16));
	if (!a)
		return -1;
	memset(a + size * *count, 0, size * (size_t)(*count ? *count : 16));
	*array = a;
	*count = *count ? *count * 2 : 16;
	return i;
}

/* ---- codecs and profiles: what the page said it decodes */
static int codec_of(VAProfile p)
{
	switch (p) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		return CODEC_H264;
	case VAProfileHEVCMain:
		return CODEC_HEVC;
	case VAProfileVP8Version0_3:
		return CODEC_VP8;
	case VAProfileVP9Profile0:
		return CODEC_VP9;
	case VAProfileAV1Profile0:
		return CODEC_AV1;
	default:
		return 0;
	}
}

static const struct codec *codec_ops(int codec)
{
	switch (codec) {
	case CODEC_H264: return &codec_h264;
	case CODEC_HEVC: return &codec_hevc;
	case CODEC_VP8: return &codec_vp8;
	case CODEC_VP9: return &codec_vp9;
	case CODEC_AV1: return &codec_av1;
	}
	return NULL;
}

/* The page's caps entry for a codec (the largest size it said yes to), or NULL. */
static const void *cap_of(struct driver *d, int codec, uint32_t *max_w, uint32_t *max_h)
{
	for (int i = 0; i < d->ncaps; i++) {
		if ((int)d->caps[i].codec == codec && (d->caps[i].flags & (CAP_HW | CAP_SW))) {
			if (max_w)
				*max_w = d->caps[i].max_w;
			if (max_h)
				*max_h = d->caps[i].max_h;
			return &d->caps[i];
		}
	}
	return NULL;
}

static int codec_offered(struct driver *d, int codec)
{
	const char *only = getenv("RVVDEC_CODECS");	/* e.g. "h264,vp9": offer only those */
	static const char *const names[] = { "", "h264", "hevc", "vp8", "vp9", "av1" };

	if (only && *only && !strstr(only, names[codec]))
		return 0;
	if (!codec_ops(codec) || !codec_ops(codec)->pack)
		return 0;	/* (not done yet) */
	if (codec == CODEC_VP8 && getenv("RVVDEC_HW_ONLY")) {
		for (int i = 0; i < d->ncaps; i++)
			if ((int)d->caps[i].codec == codec)
				return (d->caps[i].flags & CAP_HW) != 0;
	}
	return cap_of(d, codec, NULL, NULL) != NULL;
}

static VAStatus query_profiles(VADriverContextP ctx, VAProfile *list, int *n)
{
	struct driver *d = DRV(ctx);
	static const VAProfile all[] = {
		VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High, VAProfileHEVCMain,
		VAProfileVP8Version0_3, VAProfileVP9Profile0, VAProfileAV1Profile0,
	};

	*n = 0;
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (codec_offered(d, codec_of(all[i])))
			list[(*n)++] = all[i];
	return VA_STATUS_SUCCESS;
}

static VAStatus query_entrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint *list, int *n)
{
	struct driver *d = DRV(ctx);

	if (!codec_of(profile) || !codec_offered(d, codec_of(profile)))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	list[0] = VAEntrypointVLD;
	*n = 1;
	return VA_STATUS_SUCCESS;
}

static void fill_attribs(struct driver *d, int codec, VAConfigAttrib *a, int n)
{
	uint32_t mw = 0, mh = 0;

	cap_of(d, codec, &mw, &mh);
	for (int i = 0; i < n; i++) {
		switch (a[i].type) {
		case VAConfigAttribRTFormat:
			a[i].value = VA_RT_FORMAT_YUV420;
			break;
		case VAConfigAttribDecSliceMode:
			a[i].value = VA_DEC_SLICE_MODE_NORMAL;
			break;
		case VAConfigAttribMaxPictureWidth:
			a[i].value = mw;
			break;
		case VAConfigAttribMaxPictureHeight:
			a[i].value = mh;
			break;
		default:
			a[i].value = VA_ATTRIB_NOT_SUPPORTED;
		}
	}
}

static VAStatus get_config_attributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
				      VAConfigAttrib *list, int n)
{
	struct driver *d = DRV(ctx);

	if (!codec_of(profile) || !codec_offered(d, codec_of(profile)))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	fill_attribs(d, codec_of(profile), list, n);
	return VA_STATUS_SUCCESS;
}

static VAStatus create_config(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
			      VAConfigAttrib *list, int n, VAConfigID *out)
{
	struct driver *d = DRV(ctx);
	int codec = codec_of(profile), i;

	if (!codec || !codec_offered(d, codec))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	for (i = 0; i < n; i++)
		if (list[i].type == VAConfigAttribRTFormat && !(list[i].value & VA_RT_FORMAT_YUV420))
			return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
	pthread_mutex_lock(&d->lock);
	for (i = 0; i < d->nconfigs && d->configs[i].codec; i++)
		;
	if (i == d->nconfigs) {
		struct vconfig *c = realloc(d->configs, sizeof(*c) * (size_t)(d->nconfigs + 16));

		if (!c) {
			pthread_mutex_unlock(&d->lock);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
		memset(c + d->nconfigs, 0, sizeof(*c) * 16);
		d->configs = c;
		d->nconfigs += 16;
	}
	d->configs[i] = (struct vconfig){ profile, entrypoint, codec };
	pthread_mutex_unlock(&d->lock);
	*out = ID_CONFIG + (VAConfigID)i;
	return VA_STATUS_SUCCESS;
}

static VAStatus destroy_config(VADriverContextP ctx, VAConfigID id)
{
	struct driver *d = DRV(ctx);
	struct vconfig *c;

	pthread_mutex_lock(&d->lock);
	c = config_of(d, id);
	if (c)
		c->codec = 0;
	pthread_mutex_unlock(&d->lock);
	return c ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus query_config_attributes(VADriverContextP ctx, VAConfigID id, VAProfile *profile,
					VAEntrypoint *entrypoint, VAConfigAttrib *list, int *n)
{
	struct driver *d = DRV(ctx);
	struct vconfig *c = config_of(d, id);

	if (!c)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	*profile = c->profile;
	*entrypoint = c->entrypoint;
	list[0].type = VAConfigAttribRTFormat;
	list[0].value = VA_RT_FORMAT_YUV420;
	*n = 1;
	return VA_STATUS_SUCCESS;
}

/* ---- surfaces */
static void surface_gpu_free(struct driver *d, struct vsurface *s)
{
	for (int i = 0; i < 2; i++) {
		if (s->bo[i] && d->drm_fd >= 0) {
			struct vg_gem_close c = { s->bo[i], 0 };

			ioctl(d->drm_fd, DRM_IOCTL_GEM_CLOSE, &c);
		}
		s->bo[i] = s->res[i] = 0;
	}
	s->gpu = 0;
}

static VAStatus create_surfaces2(VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height,
				 VASurfaceID *out, unsigned int n, VASurfaceAttrib *attribs, unsigned int nattribs)
{
	struct driver *d = DRV(ctx);
	unsigned int i, made = 0;

	if (!(format & VA_RT_FORMAT_YUV420))
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
	for (i = 0; i < nattribs; i++) {
		if (!(attribs[i].flags & VA_SURFACE_ATTRIB_SETTABLE))
			continue;
		if (attribs[i].type == VASurfaceAttribPixelFormat && attribs[i].value.value.i != (int)VA_FOURCC_NV12)
			return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
		if (attribs[i].type == VASurfaceAttribMemoryType &&
		    attribs[i].value.value.i != VA_SURFACE_ATTRIB_MEM_TYPE_VA)
			return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;	/* (no imported buffers) */
		if (attribs[i].type == VASurfaceAttribExternalBufferDescriptor)
			return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
	}
	pthread_mutex_lock(&d->lock);
	for (i = 0; i < n; i++) {
		int k = slot_in((void **)&d->surfaces, &d->nsurfaces, sizeof(struct vsurface));

		if (k < 0 || k >= ST_MAX_SLOTS - 1)
			break;
		memset(&d->surfaces[k], 0, sizeof(d->surfaces[k]));
		d->surfaces[k].used = 1;
		d->surfaces[k].width = width;
		d->surfaces[k].height = height;
		out[i] = (VASurfaceID)k + 1;
		made++;
	}
	pthread_mutex_unlock(&d->lock);
	if (made < n) {
		vlog(d, "no more than %d surfaces\n", ST_MAX_SLOTS - 2);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	VLOG(d, "%u surfaces %ux%u (from %u)\n", n, width, height, out[0]);
	return VA_STATUS_SUCCESS;
}

static VAStatus create_surfaces(VADriverContextP ctx, int width, int height, int format, int n, VASurfaceID *out)
{
	return create_surfaces2(ctx, (unsigned)format, (unsigned)width, (unsigned)height, out, (unsigned)n, NULL, 0);
}

static VAStatus destroy_surfaces(VADriverContextP ctx, VASurfaceID *list, int n)
{
	struct driver *d = DRV(ctx);

	for (int i = 0; i < n; i++) {
		struct vsurface *s;
		uint32_t slot = list[i];

		pthread_mutex_lock(&d->lock);
		s = surface_of(d, list[i]);
		if (s) {
			surface_gpu_free(d, s);
			s->used = 0;
		}
		pthread_mutex_unlock(&d->lock);
		if (s)
			conn_cmd(d, VDEC_SLOT_END, &slot, 1);
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus query_surface_attributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib *list,
					 unsigned int *n)
{
	struct driver *d = DRV(ctx);
	struct vconfig *c = config_of(d, config);
	VASurfaceAttrib a[8];
	unsigned int k = 0;
	uint32_t mw = 4096, mh = 4096;

	if (!c)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	cap_of(d, c->codec, &mw, &mh);
	memset(a, 0, sizeof(a));
	a[k].type = VASurfaceAttribPixelFormat;
	a[k].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	a[k].value.type = VAGenericValueTypeInteger;
	a[k++].value.value.i = (int)VA_FOURCC_NV12;
	a[k].type = VASurfaceAttribMinWidth;
	a[k].flags = VA_SURFACE_ATTRIB_GETTABLE;
	a[k].value.type = VAGenericValueTypeInteger;
	a[k++].value.value.i = 16;
	a[k].type = VASurfaceAttribMinHeight;
	a[k].flags = VA_SURFACE_ATTRIB_GETTABLE;
	a[k].value.type = VAGenericValueTypeInteger;
	a[k++].value.value.i = 16;
	a[k].type = VASurfaceAttribMaxWidth;
	a[k].flags = VA_SURFACE_ATTRIB_GETTABLE;
	a[k].value.type = VAGenericValueTypeInteger;
	a[k++].value.value.i = (int)mw;
	a[k].type = VASurfaceAttribMaxHeight;
	a[k].flags = VA_SURFACE_ATTRIB_GETTABLE;
	a[k].value.type = VAGenericValueTypeInteger;
	a[k++].value.value.i = (int)mh;
	a[k].type = VASurfaceAttribMemoryType;
	a[k].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	a[k].value.type = VAGenericValueTypeInteger;
	a[k++].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA | (d->gpu ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 : 0);
	if (!list) {
		*n = k;
		return VA_STATUS_SUCCESS;
	}
	if (*n < k) {
		*n = k;
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
	}
	memcpy(list, a, sizeof(a[0]) * k);
	*n = k;
	return VA_STATUS_SUCCESS;
}

/* ---- contexts */
static VAStatus create_context(VADriverContextP ctx, VAConfigID config, int width, int height, int flag,
			       VASurfaceID *targets, int ntargets, VAContextID *out)
{
	struct driver *d = DRV(ctx);
	struct vconfig *c = config_of(d, config);
	struct vcontext *x;
	int k;

	(void)flag;
	(void)targets;
	(void)ntargets;
	if (!c)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	pthread_mutex_lock(&d->lock);
	k = slot_in((void **)&d->contexts, &d->ncontexts, sizeof(struct vcontext));
	if (k < 0) {
		pthread_mutex_unlock(&d->lock);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	x = &d->contexts[k];
	memset(x, 0, sizeof(*x));
	x->used = 1;
	x->stream = ++d->next_stream;
	x->cfg = *c;
	x->codec = codec_ops(c->codec);
	x->width = width;
	x->height = height;
	x->drv = d;
	pthread_mutex_unlock(&d->lock);
	*out = ID_CONTEXT + (VAContextID)k;
	VLOG(d, "context %u: stream %u, codec %d, %dx%d\n", *out, x->stream, c->codec, width, height);
	return VA_STATUS_SUCCESS;
}

static VAStatus destroy_context(VADriverContextP ctx, VAContextID id)
{
	struct driver *d = DRV(ctx);
	struct vcontext *x;
	uint32_t stream;
	int started;

	pthread_mutex_lock(&d->lock);
	x = context_of(d, id);
	if (!x) {
		pthread_mutex_unlock(&d->lock);
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	}
	stream = x->stream;
	started = x->started;
	if (x->codec && x->codec->destroy)
		x->codec->destroy(x);
	free(x->pic);
	x->used = 0;
	pthread_mutex_unlock(&d->lock);
	if (started)
		conn_cmd(d, VDEC_STREAM_END, &stream, 1);
	return VA_STATUS_SUCCESS;
}

/* ---- buffers */
static int buffer_new(struct driver *d, struct vbuf *b, VABufferID *out)
{
	int i;

	for (i = 0; i < d->nbuffers && d->buffers[i]; i++)
		;
	if (i == d->nbuffers) {
		struct vbuf **n = realloc(d->buffers, sizeof(*n) * (size_t)(d->nbuffers ? d->nbuffers * 2 : 256));

		if (!n)
			return -1;
		memset(n + d->nbuffers, 0, sizeof(*n) * (size_t)(d->nbuffers ? d->nbuffers : 256));
		d->buffers = n;
		d->nbuffers = d->nbuffers ? d->nbuffers * 2 : 256;
	}
	d->buffers[i] = b;
	*out = ID_BUFFER + (VABufferID)i;
	return 0;
}

static VAStatus create_buffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size,
			      unsigned int num, void *data, VABufferID *out)
{
	struct driver *d = DRV(ctx);
	struct vbuf *b;

	(void)context;
	if (type == VAImageBufferType)
		return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;	/* (vaCreateImage's own) */
	if ((uint64_t)size * num > (256u << 20))
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	b = calloc(1, sizeof(*b));
	if (!b || !(b->data = malloc((size_t)size * num + 1))) {
		free(b);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	b->type = type;
	b->size = size;
	b->num = num;
	b->mem = -1;
	if (data)
		memcpy(b->data, data, (size_t)size * num);
	pthread_mutex_lock(&d->lock);
	if (buffer_new(d, b, out)) {
		pthread_mutex_unlock(&d->lock);
		free(b->data);
		free(b);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	pthread_mutex_unlock(&d->lock);
	return VA_STATUS_SUCCESS;
}

static VAStatus buffer_set_num_elements(VADriverContextP ctx, VABufferID id, unsigned int num)
{
	struct driver *d = DRV(ctx);
	struct vbuf *b = buffer_of(d, id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	if (num > b->num)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	b->num = num;
	return VA_STATUS_SUCCESS;
}

static VAStatus map_buffer(VADriverContextP ctx, VABufferID id, void **p)
{
	struct driver *d = DRV(ctx);
	struct vbuf *b = buffer_of(d, id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	*p = b->data;
	b->mapped = 1;
	return VA_STATUS_SUCCESS;
}

static VAStatus unmap_buffer(VADriverContextP ctx, VABufferID id)
{
	struct driver *d = DRV(ctx);
	struct vbuf *b = buffer_of(d, id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	b->mapped = 0;
	return VA_STATUS_SUCCESS;
}

/* An image's memory file back to the ones kept, or to gpuserver. */
static void mem_release(struct driver *d, struct mem *m)
{
	if (d->nkeep < (int)(sizeof(d->keep) / sizeof(d->keep[0]))) {
		d->keep[d->nkeep++] = *m;
		return;
	}
	conn_free(d, m);
}

static void buffer_free(struct driver *d, struct vbuf *b)
{
	if (b->mem >= 0) {
		struct mem m = { b->mem, b->mem_fd, b->data, b->mem_size };

		mem_release(d, &m);
	} else {
		free(b->data);
	}
	free(b);
}

static VAStatus destroy_buffer(VADriverContextP ctx, VABufferID id)
{
	struct driver *d = DRV(ctx);
	struct vbuf *b;

	pthread_mutex_lock(&d->lock);
	b = buffer_of(d, id);
	if (b)
		d->buffers[id - ID_BUFFER] = NULL;
	pthread_mutex_unlock(&d->lock);
	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	buffer_free(d, b);
	return VA_STATUS_SUCCESS;
}

static VAStatus buffer_info(VADriverContextP ctx, VABufferID id, VABufferType *type, unsigned int *size,
			    unsigned int *num)
{
	struct driver *d = DRV(ctx);
	struct vbuf *b = buffer_of(d, id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	*type = b->type;
	*size = b->size;
	*num = b->num;
	return VA_STATUS_SUCCESS;
}

/* ---- decoding */
static VAStatus begin_picture(VADriverContextP ctx, VAContextID context, VASurfaceID target)
{
	struct driver *d = DRV(ctx);
	struct vcontext *x = context_of(d, context);

	if (!x)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (!surface_of(d, target))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	x->target = target;
	x->npic = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus render_picture(VADriverContextP ctx, VAContextID context, VABufferID *list, int n)
{
	struct driver *d = DRV(ctx);
	struct vcontext *x = context_of(d, context);

	if (!x)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (x->npic + n > x->cappic) {
		int cap = x->cappic ? x->cappic : 64;
		struct vbuf **p;

		while (cap < x->npic + n)
			cap *= 2;
		p = realloc(x->pic, sizeof(*p) * (size_t)cap);
		if (!p)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		x->pic = p;
		x->cappic = cap;
	}
	for (int i = 0; i < n; i++) {
		struct vbuf *b = buffer_of(d, list[i]);

		if (!b)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		x->pic[x->npic++] = b;
	}
	return VA_STATUS_SUCCESS;
}

struct room_arg { uint32_t end; };

static int room_there(struct driver *d, void *arg)
{
	struct room_arg *a = arg;

	return (uint32_t)(a->end - st_word(d, ST_CONSUMED)) <= d->chunk.size;
}

static int all_taken(struct driver *d, void *arg)
{
	(void)arg;
	return st_word(d, ST_CONSUMED) == d->written;
}

/*
 * A chunk into the chunk area (a ring: a chunk is never split; what is left at the end is skipped, which
 * the page's ST_CONSUMED passes over since every DECODE says where its chunk ends). Returns its offset.
 */
static int chunk_place(struct driver *d, const uint8_t *p, size_t n, uint32_t *offset, uint32_t *end)
{
	uint32_t pos, skip;
	struct room_arg a;

	if (!d->chunk.p || n > d->chunk.size) {
		uint32_t size = CHUNK_AREA;

		while (size < 2 * n)
			size *= 2;
		if (d->chunk.p) {
			if (st_wait(d, all_taken, NULL, d->timeout_ms))
				return -1;
			conn_free(d, &d->chunk);
		}
		if (conn_alloc(d, size, &d->chunk))
			return -1;
		/* (a new area: its offset 0 is where the count stands now) */
		d->chunk_base = d->written;
	}
	pos = (d->written - d->chunk_base) % d->chunk.size;
	skip = pos + n > d->chunk.size ? d->chunk.size - pos : 0;
	a.end = d->written + skip + (uint32_t)n;
	if (!room_there(d, &a) && st_wait(d, room_there, &a, d->timeout_ms)) {
		vlog(d, "no room for a chunk of %zu bytes: the page takes none\n", n);
		return -1;
	}
	pos = skip ? 0 : pos;
	memcpy(d->chunk.p + pos, p, n);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	d->written = a.end;
	*offset = pos;
	*end = a.end;
	return 0;
}

/* The picture's chunks out: the stream said first (with its first chunk), then each chunk. */
static VAStatus emit(struct driver *d, struct vcontext *x, struct vsurface *s, uint32_t slot, struct chunks *out)
{
	if (!x->started) {
		uint32_t a[7], profile = 0, level = 0;

		if (x->codec->describe)
			x->codec->describe(x, &profile, &level);
		a[0] = x->stream;
		a[1] = (uint32_t)x->cfg.codec;
		a[2] = profile;
		a[3] = level;
		a[4] = (uint32_t)x->width;
		a[5] = (uint32_t)x->height;
		a[6] = 0;
		if (conn_cmd(d, VDEC_STREAM, a, 7))
			return VA_STATUS_ERROR_OPERATION_FAILED;
		x->started = 1;
	}
	for (int i = 0; i < out->n; i++) {
		uint32_t a[8], offset, end, seq = ++d->seq ? d->seq : ++d->seq;
		struct vsurface *t = out->c[i].slot ? surface_of(d, out->c[i].slot) : s;
		uint32_t to = out->c[i].slot && t ? out->c[i].slot : slot, flags;

		if (!t)
			t = s;
		flags = out->c[i].flags | (t->gpu ? CHUNK_DRAW : 0);
		const uint8_t *p = out->b.p + out->c[i].at;
		size_t n = out->c[i].size;

		if (d->dump) {
			uint32_t h[4] = { x->stream, (uint32_t)x->cfg.codec, flags, (uint32_t)n };

			fwrite(h, sizeof(h), 1, d->dump);
			fwrite(p, n, 1, d->dump);
			fflush(d->dump);
		}
		if (chunk_place(d, p, n, &offset, &end))
			return VA_STATUS_ERROR_OPERATION_FAILED;
		a[0] = x->stream;
		a[1] = to;
		a[2] = seq;
		a[3] = (uint32_t)d->chunk.id;
		a[4] = offset;
		a[5] = (uint32_t)n;
		a[6] = flags;
		a[7] = end;
		if (conn_cmd(d, VDEC_DECODE, a, 8))
			return VA_STATUS_ERROR_OPERATION_FAILED;
		if (!(flags & CHUNK_HIDDEN)) {
			t->seq = seq;
			t->pending = 1;
		}
		d->st.chunks++;
		d->st.keys += (flags & CHUNK_KEY) != 0;
		d->st.hidden += (flags & CHUNK_HIDDEN) != 0;
		d->st.bytes += n;
		VLOG(d, "stream %u: chunk %u (%zu bytes%s%s) for surface %u\n", x->stream, seq, n,
		     flags & CHUNK_KEY ? ", key" : "", flags & CHUNK_HIDDEN ? ", hidden" : "", to);
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus end_picture(VADriverContextP ctx, VAContextID context)
{
	struct driver *d = DRV(ctx);
	struct vcontext *x = context_of(d, context);
	struct vsurface *s;
	struct chunks out;
	VAStatus st;

	if (!x)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	s = surface_of(d, x->target);
	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	memset(&out, 0, sizeof(out));
	pthread_mutex_lock(&d->chunk_lock);
	st = x->codec->pack(x, x->pic, x->npic, &out);
	x->npic = 0;
	if (st == VA_STATUS_SUCCESS && !out.n)
		st = VA_STATUS_ERROR_DECODING_ERROR;
	if (st == VA_STATUS_SUCCESS)
		st = emit(d, x, s, x->target, &out);
	if (st != VA_STATUS_SUCCESS) {
		/* (the surface has no picture: a wait on it is over at once) */
		s->pending = 0;
		s->seq = 0;
	}
	pthread_mutex_unlock(&d->chunk_lock);
	free(out.b.p);
	return st;
}

struct sync_arg { uint32_t slot, seq; int gpu; };

static int synced(struct driver *d, void *arg)
{
	struct sync_arg *a = arg;
	uint32_t *w = st_slot(d, a->slot);
	uint32_t decoded = __atomic_load_n(&w[0], __ATOMIC_ACQUIRE), ready = __atomic_load_n(&w[1], __ATOMIC_ACQUIRE);
	uint32_t error = __atomic_load_n(&w[4], __ATOMIC_ACQUIRE);

	if (error && seq_reached(error, a->seq))
		return 1;
	return seq_reached(decoded, a->seq) && (!a->gpu || seq_reached(ready, a->seq));
}

static VAStatus sync_with(struct driver *d, VASurfaceID id, int timeout_ms)
{
	struct vsurface *s = surface_of(d, id);
	struct sync_arg a;
	uint32_t *w;

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!s->pending)
		return VA_STATUS_SUCCESS;
	a.slot = id;
	a.seq = s->seq;
	a.gpu = s->gpu;
	{
		long t0 = now_us();
		int r = synced(d, &a) ? 0 : st_wait(d, synced, &a, timeout_ms);
		uint32_t us = (uint32_t)(now_us() - t0);

		d->st.waits++;
		d->st.wait_us += us;
		if (us > d->st.wait_max_us)
			d->st.wait_max_us = us;
		if (r) {
			d->st.timeouts++;
			VLOG(d, "surface %u: chunk %u not there in time\n", id, a.seq);
			return VA_STATUS_ERROR_TIMEDOUT;
		}
	}
	w = st_slot(d, id);
	if (w[4] && seq_reached(w[4], a.seq)) {
		d->st.errors++;
		VLOG(d, "surface %u: chunk %u failed on the page\n", id, a.seq);
		s->pending = 0;
		return VA_STATUS_ERROR_DECODING_ERROR;
	}
	if (s->seq == a.seq)
		s->pending = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus sync_surface(VADriverContextP ctx, VASurfaceID id)
{
	struct driver *d = DRV(ctx);

	return sync_with(d, id, d->timeout_ms);
}

static VAStatus sync_surface2(VADriverContextP ctx, VASurfaceID id, uint64_t timeout_ns)
{
	struct driver *d = DRV(ctx);
	uint64_t ms = timeout_ns / 1000000;

	return sync_with(d, id, timeout_ns == VA_TIMEOUT_INFINITE || ms > (uint64_t)d->timeout_ms ? d->timeout_ms : (int)ms);
}

static VAStatus query_surface_status(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus *status)
{
	struct driver *d = DRV(ctx);
	struct vsurface *s = surface_of(d, id);
	struct sync_arg a;

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	a.slot = id;
	a.seq = s->seq;
	a.gpu = s->gpu;
	*status = !s->pending || synced(d, &a) ? VASurfaceReady : VASurfaceRendering;
	return VA_STATUS_SUCCESS;
}

static VAStatus query_surface_error(VADriverContextP ctx, VASurfaceID id, VAStatus error, void **info)
{
	(void)ctx;
	(void)id;
	(void)error;
	*info = NULL;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* ---- images (NV12 only) */
static VAStatus query_image_formats(VADriverContextP ctx, VAImageFormat *list, int *n)
{
	(void)ctx;
	memset(&list[0], 0, sizeof(list[0]));
	list[0].fourcc = VA_FOURCC_NV12;
	list[0].byte_order = VA_LSB_FIRST;
	list[0].bits_per_pixel = 12;
	*n = 1;
	return VA_STATUS_SUCCESS;
}

/* A memory file of at least `size` bytes: one kept, or a new one. */
static int image_mem(struct driver *d, uint32_t size, struct mem *m)
{
	for (int i = 0; i < d->nkeep; i++) {
		if (d->keep[i].size >= size && d->keep[i].size <= size + size / 4) {
			*m = d->keep[i];
			d->keep[i] = d->keep[--d->nkeep];
			return 0;
		}
	}
	return conn_alloc(d, size, m);
}

static VAStatus create_image(VADriverContextP ctx, VAImageFormat *format, int width, int height, VAImage *out)
{
	struct driver *d = DRV(ctx);
	uint32_t pitch, h2, size;
	struct vbuf *b;
	struct mem m;
	int k;

	if (!format || format->fourcc != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
	if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	pitch = ALIGN((uint32_t)width, 64);
	h2 = ALIGN((uint32_t)height, 2);
	size = pitch * h2 + pitch * h2 / 2;
	if (image_mem(d, size, &m))
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	b = calloc(1, sizeof(*b));
	if (!b) {
		mem_release(d, &m);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	b->type = VAImageBufferType;
	b->size = size;
	b->num = 1;
	b->data = m.p;
	b->mem = m.id;
	b->mem_fd = m.fd;
	b->mem_size = m.size;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&d->lock);
	k = slot_in((void **)&d->images, &d->nimages, sizeof(struct vimage));
	if (k < 0 || buffer_new(d, b, &out->buf)) {
		pthread_mutex_unlock(&d->lock);
		buffer_free(d, b);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	out->image_id = ID_IMAGE + (VAImageID)k;
	out->format = *format;
	out->width = (uint16_t)width;
	out->height = (uint16_t)height;
	out->data_size = size;
	out->num_planes = 2;
	out->pitches[0] = pitch;
	out->pitches[1] = pitch;
	out->offsets[0] = 0;
	out->offsets[1] = pitch * h2;
	d->images[k].used = 1;
	d->images[k].image = *out;
	pthread_mutex_unlock(&d->lock);
	return VA_STATUS_SUCCESS;
}

static VAStatus derive_image(VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
	(void)ctx;
	(void)surface;
	(void)image;
	/* (a surface's pixels are not in the machine; FFmpeg then copies with vaGetImage) */
	return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus destroy_image(VADriverContextP ctx, VAImageID id)
{
	struct driver *d = DRV(ctx);
	struct vimage *im;
	VABufferID buf;

	pthread_mutex_lock(&d->lock);
	im = image_of(d, id);
	if (!im) {
		pthread_mutex_unlock(&d->lock);
		return VA_STATUS_ERROR_INVALID_IMAGE;
	}
	im->used = 0;
	buf = im->image.buf;
	pthread_mutex_unlock(&d->lock);
	return destroy_buffer(ctx, buf);
}

static VAStatus set_image_palette(VADriverContextP ctx, VAImageID image, unsigned char *palette)
{
	(void)ctx;
	(void)image;
	(void)palette;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

struct copy_arg { uint32_t slot, id; };

static int copied(struct driver *d, void *arg)
{
	struct copy_arg *a = arg;

	return __atomic_load_n(&st_slot(d, a->slot)[2], __ATOMIC_ACQUIRE) == a->id;
}

static VAStatus get_image(VADriverContextP ctx, VASurfaceID surface, int x, int y, unsigned int width,
			  unsigned int height, VAImageID id)
{
	struct driver *d = DRV(ctx);
	struct vsurface *s = surface_of(d, surface);
	struct vimage *im = image_of(d, id);
	struct copy_arg ca;
	struct vbuf *b;
	uint32_t a[11];
	VAStatus st;

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!im || !(b = buffer_of(d, im->image.buf)))
		return VA_STATUS_ERROR_INVALID_IMAGE;
	if (x || y || width > im->image.width || height > im->image.height)
		return VA_STATUS_ERROR_INVALID_PARAMETER;	/* (whole pictures only) */
	if (!s->seq) {
		/* never decoded into: black */
		memset(b->data, 16, im->image.offsets[1]);
		memset(b->data + im->image.offsets[1], 128, b->size - im->image.offsets[1]);
		return VA_STATUS_SUCCESS;
	}
	st = sync_with(d, surface, d->timeout_ms);
	if (st != VA_STATUS_SUCCESS)
		return st;
	ca.slot = surface;
	ca.id = ++d->next_copy ? d->next_copy : ++d->next_copy;
	a[0] = surface;
	a[1] = ca.id;
	a[2] = s->seq;
	a[3] = (uint32_t)b->mem;
	a[4] = im->image.offsets[0];
	a[5] = im->image.pitches[0];
	a[6] = im->image.offsets[1];
	a[7] = im->image.pitches[1];
	a[8] = width;
	a[9] = height;
	a[10] = b->size;
	if (conn_cmd(d, VDEC_COPY, a, 11))
		return VA_STATUS_ERROR_OPERATION_FAILED;
	{
		long t0 = now_us();

		if (st_wait(d, copied, &ca, d->timeout_ms)) {
			d->st.timeouts++;
			VLOG(d, "surface %u: copy %u not done in time\n", surface, ca.id);
			return VA_STATUS_ERROR_TIMEDOUT;
		}
		d->st.copies++;
		d->st.copy_us += (uint64_t)(now_us() - t0);
	}
	if (st_slot(d, surface)[3]) {
		VLOG(d, "surface %u: copy %u failed on the page (%u)\n", surface, ca.id, st_slot(d, surface)[3]);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus put_image(VADriverContextP ctx, VASurfaceID surface, VAImageID image, int sx, int sy,
			  unsigned int sw, unsigned int sh, int dx, int dy, unsigned int dw, unsigned int dh)
{
	struct driver *d = DRV(ctx);
	static int said;

	(void)surface; (void)image; (void)sx; (void)sy; (void)sw; (void)sh; (void)dx; (void)dy; (void)dw; (void)dh;
	/* (mpv uploads a small picture to try export at its start: taken, not kept; surfaces hold decoded frames) */
	if (!said++)
		VLOG(d, "vaPutImage: the picture is not kept (uploads are not done here)\n");
	return VA_STATUS_SUCCESS;
}

/* ---- export (zero copy) */
static int gpu_resource(struct driver *d, uint32_t format, uint32_t w, uint32_t h, uint32_t *bo, uint32_t *res)
{
	struct vg_resource_create rc;

	memset(&rc, 0, sizeof(rc));
	rc.target = PIPE_TEXTURE_2D;
	rc.format = format;
	rc.bind = VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET;
	rc.width = w;
	rc.height = h;
	rc.depth = 1;
	rc.array_size = 1;
	rc.size = BACKING_SIZE;
	if (ioctl(d->drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc)) {
		vlog(d, "GPU resource %ux%u: %s\n", w, h, strerror(errno));
		return -1;
	}
	*bo = rc.bo_handle;
	*res = rc.res_handle;
	return 0;
}

static VAStatus export_surface(VADriverContextP ctx, VASurfaceID id, uint32_t mem_type, uint32_t flags,
			       void *descriptor)
{
	struct driver *d = DRV(ctx);
	VADRMPRIMESurfaceDescriptor *desc = descriptor;
	struct vsurface *s = surface_of(d, id);
	uint32_t a[5], cw, ch;
	int fds[2] = { -1, -1 };

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
	if (!d->gpu || d->drm_fd < 0) {
		VLOG(d, "export: no GPU on the page (or no render node): copies only (vaapi-copy)\n");
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
	}
	if (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) {
		/* (one NV12 layer: the window server cannot show NV12 buffers) */
		VLOG(d, "export: composed layers are not done (separate layers are)\n");
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
	cw = (s->width + 1) / 2;
	ch = (s->height + 1) / 2;
	pthread_mutex_lock(&d->lock);
	if (!s->bo[0]) {
		if (gpu_resource(d, VIRGL_FORMAT_R8_UNORM, s->width, s->height, &s->bo[0], &s->res[0]) ||
		    gpu_resource(d, VIRGL_FORMAT_R8G8_UNORM, cw, ch, &s->bo[1], &s->res[1])) {
			surface_gpu_free(d, s);
			pthread_mutex_unlock(&d->lock);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
		a[0] = id;
		a[1] = s->width;
		a[2] = s->height;
		a[3] = s->res[0];
		a[4] = s->res[1];
		if (conn_cmd(d, VDEC_SURFACE, a, 5)) {
			surface_gpu_free(d, s);
			pthread_mutex_unlock(&d->lock);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
		VLOG(d, "surface %u: GPU resources %u and %u\n", id, s->res[0], s->res[1]);
	}
	if (!s->gpu) {
		/* From now on every frame decoded into it is drawn; the one it has now is drawn at once. */
		s->gpu = 1;
		a[0] = id;
		if (conn_cmd(d, VDEC_EXPORT, a, 1)) {
			pthread_mutex_unlock(&d->lock);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
		if (s->seq)
			s->pending = 1;
	}
	d->st.exports++;
	for (int i = 0; i < 2; i++) {
		struct vg_prime p = { s->bo[i], DRM_CLOEXEC | DRM_RDWR, -1 };

		if (ioctl(d->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p) || p.fd < 0) {
			vlog(d, "export: PRIME: %s\n", strerror(errno));
			if (fds[0] >= 0)
				close(fds[0]);
			pthread_mutex_unlock(&d->lock);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
		fds[i] = p.fd;
	}
	pthread_mutex_unlock(&d->lock);
	memset(desc, 0, sizeof(*desc));
	desc->fourcc = VA_FOURCC_NV12;
	desc->width = s->width;
	desc->height = s->height;
	desc->num_objects = 2;
	for (int i = 0; i < 2; i++) {
		desc->objects[i].fd = fds[i];
		desc->objects[i].size = BACKING_SIZE;
		desc->objects[i].drm_format_modifier = 0;	/* DRM_FORMAT_MOD_LINEAR */
	}
	desc->num_layers = 2;
	desc->layers[0].drm_format = DRM_FORMAT_R8;
	desc->layers[0].num_planes = 1;
	desc->layers[0].object_index[0] = 0;
	desc->layers[0].pitch[0] = s->width;
	desc->layers[1].drm_format = DRM_FORMAT_GR88;
	desc->layers[1].num_planes = 1;
	desc->layers[1].object_index[0] = 1;
	desc->layers[1].pitch[0] = 2 * cw;
	return VA_STATUS_SUCCESS;
}

/* ---- what is not done */
static VAStatus put_surface(VADriverContextP ctx, VASurfaceID s, void *draw, short sx, short sy, unsigned short sw,
			    unsigned short sh, short dx, short dy, unsigned short dw, unsigned short dh,
			    VARectangle *clip, unsigned int nclip, unsigned int flags)
{
	(void)ctx; (void)s; (void)draw; (void)sx; (void)sy; (void)sw; (void)sh; (void)dx; (void)dy; (void)dw;
	(void)dh; (void)clip; (void)nclip; (void)flags;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus query_subpicture_formats(VADriverContextP ctx, VAImageFormat *list, unsigned int *flags,
					 unsigned int *n)
{
	(void)ctx; (void)list; (void)flags;
	*n = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus create_subpicture(VADriverContextP ctx, VAImageID image, VASubpictureID *s)
{
	(void)ctx; (void)image; (void)s;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus destroy_subpicture(VADriverContextP ctx, VASubpictureID s)
{
	(void)ctx; (void)s;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus set_subpicture_image(VADriverContextP ctx, VASubpictureID s, VAImageID image)
{
	(void)ctx; (void)s; (void)image;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus set_subpicture_chromakey(VADriverContextP ctx, VASubpictureID s, unsigned int min, unsigned int max,
					 unsigned int mask)
{
	(void)ctx; (void)s; (void)min; (void)max; (void)mask;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus set_subpicture_global_alpha(VADriverContextP ctx, VASubpictureID s, float alpha)
{
	(void)ctx; (void)s; (void)alpha;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus associate_subpicture(VADriverContextP ctx, VASubpictureID s, VASurfaceID *list, int n, short sx,
				     short sy, unsigned short sw, unsigned short sh, short dx, short dy,
				     unsigned short dw, unsigned short dh, unsigned int flags)
{
	(void)ctx; (void)s; (void)list; (void)n; (void)sx; (void)sy; (void)sw; (void)sh; (void)dx; (void)dy;
	(void)dw; (void)dh; (void)flags;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus deassociate_subpicture(VADriverContextP ctx, VASubpictureID s, VASurfaceID *list, int n)
{
	(void)ctx; (void)s; (void)list; (void)n;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus query_display_attributes(VADriverContextP ctx, VADisplayAttribute *list, int *n)
{
	(void)ctx; (void)list;
	*n = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus get_display_attributes(VADriverContextP ctx, VADisplayAttribute *list, int n)
{
	(void)ctx; (void)list; (void)n;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus set_display_attributes(VADriverContextP ctx, VADisplayAttribute *list, int n)
{
	(void)ctx; (void)list; (void)n;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus lock_surface(VADriverContextP ctx, VASurfaceID s, unsigned int *fourcc, unsigned int *luma_stride,
			     unsigned int *chroma_u_stride, unsigned int *chroma_v_stride, unsigned int *luma_offset,
			     unsigned int *chroma_u_offset, unsigned int *chroma_v_offset, unsigned int *buffer_name,
			     void **buffer)
{
	(void)ctx; (void)s; (void)fourcc; (void)luma_stride; (void)chroma_u_stride; (void)chroma_v_stride;
	(void)luma_offset; (void)chroma_u_offset; (void)chroma_v_offset; (void)buffer_name; (void)buffer;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus unlock_surface(VADriverContextP ctx, VASurfaceID s)
{
	(void)ctx; (void)s;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* ---- the start and the end */
static VAStatus terminate(VADriverContextP ctx)
{
	struct driver *d = DRV(ctx);

	if (!d)
		return VA_STATUS_SUCCESS;
	if (d->st.chunks && !(getenv("RVVDEC_STATS") && !atoi(getenv("RVVDEC_STATS"))))
		vlog(d, "%llu chunks to the page (%llu key, %llu hidden, %.1f MiB); %llu waits for a frame, %.1f ms on average, "
		     "%.1f at most; %llu copies (%.1f ms each), %llu exports; %llu errors, %llu timeouts\n",
		     (unsigned long long)d->st.chunks, (unsigned long long)d->st.keys, (unsigned long long)d->st.hidden,
		     d->st.bytes / 1048576.0, (unsigned long long)d->st.waits,
		     d->st.waits ? d->st.wait_us / 1000.0 / d->st.waits : 0.0, d->st.wait_max_us / 1000.0,
		     (unsigned long long)d->st.copies, d->st.copies ? d->st.copy_us / 1000.0 / d->st.copies : 0.0,
		     (unsigned long long)d->st.exports, (unsigned long long)d->st.errors, (unsigned long long)d->st.timeouts);
	for (int i = 0; i < d->ncontexts; i++)
		if (d->contexts[i].used)
			destroy_context(ctx, ID_CONTEXT + (VAContextID)i);
	for (int i = 0; i < d->nsurfaces; i++)
		if (d->surfaces[i].used)
			surface_gpu_free(d, &d->surfaces[i]);
	for (int i = 0; i < d->nbuffers; i++)
		if (d->buffers[i])
			buffer_free(d, d->buffers[i]);
	for (int i = 0; i < d->nkeep; i++)
		conn_free(d, &d->keep[i]);
	conn_free(d, &d->chunk);
	conn_close(d);
	if (d->dump)
		fclose(d->dump);
	free(d->configs);
	free(d->contexts);
	free(d->surfaces);
	free(d->buffers);
	free(d->images);
	pthread_mutex_destroy(&d->lock);
	pthread_mutex_destroy(&d->sock_lock);
	pthread_mutex_destroy(&d->chunk_lock);
	free(d);
	ctx->pDriverData = NULL;
	return VA_STATUS_SUCCESS;
}

/* __vaDriverInit_1_<minor> of the headers built with; libva looks for its own minor and every one below it */
#define CAT(a, b) a##b
#define INIT_NAME(minor) CAT(__vaDriverInit_1_, minor)
VAStatus __attribute__((visibility("default"))) INIT_NAME(VA_MINOR_VERSION)(VADriverContextP ctx);

VAStatus INIT_NAME(VA_MINOR_VERSION)(VADriverContextP ctx)
{
	struct VADriverVTable *vt = ctx->vtable;
	struct drm_state *drm = ctx->drm_state;
	struct driver *d = calloc(1, sizeof(*d));
	const char *dump = getenv("RVVDEC_DUMP");

	if (!d)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	pthread_mutex_init(&d->lock, NULL);
	pthread_mutex_init(&d->sock_lock, NULL);
	pthread_mutex_init(&d->chunk_lock, NULL);
	d->sock = d->status_fd = -1;
	d->chunk.fd = -1;
	d->verbose = getenv("RVVDEC_DEBUG") && atoi(getenv("RVVDEC_DEBUG"));
	d->timeout_ms = getenv("RVVDEC_TIMEOUT") ? atoi(getenv("RVVDEC_TIMEOUT")) : 10000;
	d->drm_fd = drm && drm->fd >= 0 ? drm->fd : -1;
	ctx->pDriverData = d;
	if (conn_open(d)) {
		terminate(ctx);
		return VA_STATUS_ERROR_UNKNOWN;
	}
	d->gpu = d->ncaps && (d->caps[0].flags & CAP_GPU) && d->drm_fd >= 0 && !getenv("RVVDEC_NO_GPU");
	if (dump && *dump && !(d->dump = fopen(dump, "wb")))
		vlog(d, "RVVDEC_DUMP %s: %s\n", dump, strerror(errno));
	if (d->verbose) {
		vlog(d, "the page decodes:");
		for (int i = 0; i < d->ncaps; i++)
			fprintf(stderr, " %u/%u %ux%u%s%s", d->caps[i].codec, d->caps[i].profile, d->caps[i].max_w,
				d->caps[i].max_h, d->caps[i].flags & CAP_HW ? " hw" : "", d->caps[i].flags & CAP_SW ? " sw" : "");
		fprintf(stderr, "; zero copy %s\n", d->gpu ? "yes" : "no");
	}

	ctx->version_major = VA_MAJOR_VERSION;
	ctx->version_minor = VA_MINOR_VERSION;
	ctx->max_profiles = MAX_PROFILES;
	ctx->max_entrypoints = 1;
	ctx->max_attributes = 8;
	ctx->max_image_formats = 1;
	ctx->max_subpic_formats = 1;
	ctx->max_display_attributes = 1;
	ctx->str_vendor = VENDOR;

	vt->vaTerminate = terminate;
	vt->vaQueryConfigProfiles = query_profiles;
	vt->vaQueryConfigEntrypoints = query_entrypoints;
	vt->vaGetConfigAttributes = get_config_attributes;
	vt->vaCreateConfig = create_config;
	vt->vaDestroyConfig = destroy_config;
	vt->vaQueryConfigAttributes = query_config_attributes;
	vt->vaCreateSurfaces = create_surfaces;
	vt->vaCreateSurfaces2 = create_surfaces2;
	vt->vaDestroySurfaces = destroy_surfaces;
	vt->vaQuerySurfaceAttributes = query_surface_attributes;
	vt->vaCreateContext = create_context;
	vt->vaDestroyContext = destroy_context;
	vt->vaCreateBuffer = create_buffer;
	vt->vaBufferSetNumElements = buffer_set_num_elements;
	vt->vaMapBuffer = map_buffer;
	vt->vaUnmapBuffer = unmap_buffer;
	vt->vaDestroyBuffer = destroy_buffer;
	vt->vaBufferInfo = buffer_info;
	vt->vaBeginPicture = begin_picture;
	vt->vaRenderPicture = render_picture;
	vt->vaEndPicture = end_picture;
	vt->vaSyncSurface = sync_surface;
	vt->vaSyncSurface2 = sync_surface2;
	vt->vaQuerySurfaceStatus = query_surface_status;
	vt->vaQuerySurfaceError = query_surface_error;
	vt->vaPutSurface = put_surface;
	vt->vaQueryImageFormats = query_image_formats;
	vt->vaCreateImage = create_image;
	vt->vaDeriveImage = derive_image;
	vt->vaDestroyImage = destroy_image;
	vt->vaSetImagePalette = set_image_palette;
	vt->vaGetImage = get_image;
	vt->vaPutImage = put_image;
	vt->vaQuerySubpictureFormats = query_subpicture_formats;
	vt->vaCreateSubpicture = create_subpicture;
	vt->vaDestroySubpicture = destroy_subpicture;
	vt->vaSetSubpictureImage = set_subpicture_image;
	vt->vaSetSubpictureChromakey = set_subpicture_chromakey;
	vt->vaSetSubpictureGlobalAlpha = set_subpicture_global_alpha;
	vt->vaAssociateSubpicture = associate_subpicture;
	vt->vaDeassociateSubpicture = deassociate_subpicture;
	vt->vaQueryDisplayAttributes = query_display_attributes;
	vt->vaGetDisplayAttributes = get_display_attributes;
	vt->vaSetDisplayAttributes = set_display_attributes;
	vt->vaLockSurface = lock_surface;
	vt->vaUnlockSurface = unlock_surface;
	vt->vaExportSurfaceHandle = export_surface;
	return VA_STATUS_SUCCESS;
}
