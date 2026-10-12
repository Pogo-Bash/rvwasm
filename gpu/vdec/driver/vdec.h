/* SPDX-License-Identifier: MIT */
/*
 * rvwasm_drv_video.so: a VA-API driver whose decoder is the page's WebCodecs (docs/video-decode.md).
 *
 * The driver turns VA's stateless buffers into stateful chunks (H.264 Annex B with parameter sets made from
 * VA's parameters, HEVC Annex B with parameter sets and rewritten slice headers, VP9 and VP8 frames, AV1
 * temporal units with sequence and frame headers), writes them into a memory file it shares with gpuserver,
 * and says so on gpuserver's socket. The page decodes them and writes status words into another shared
 * memory file, which the driver reads to know when a surface is done; images (vaGetImage) are memory files
 * the page copies frames into; vaExportSurfaceHandle hands out two GPU resources of the render node that the
 * page draws each picture into.
 */
#ifndef RVVDEC_H
#define RVVDEC_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <va/va.h>
#include <va/va_backend.h>

/* ---- the socket (gpuserver's, vtest framing; gpu/server/vdec.js) */
#define VDEC_HELLO		0x10010
#define VDEC_ALLOC		0x10011
#define VDEC_FREE		0x10012
#define VDEC_STREAM		0x10013
#define VDEC_STREAM_END		0x10014
#define VDEC_SURFACE		0x10015
#define VDEC_DECODE		0x10016
#define VDEC_COPY		0x10017
#define VDEC_EXPORT		0x10018
#define VDEC_SLOT_END		0x10019
#define VDEC_VERSION		1

/* codecs (the socket's and the page's numbers) */
#define CODEC_H264	1
#define CODEC_HEVC	2
#define CODEC_VP8	3
#define CODEC_VP9	4
#define CODEC_AV1	5

/* chunk flags */
#define CHUNK_KEY	1	/* a key chunk (WebCodecs "key") */
#define CHUNK_HIDDEN	2	/* gives no frame */
#define CHUNK_DRAW	4	/* draw the frame into the slot's GPU resources */

/* cap flags */
#define CAP_HW		1	/* VideoToolbox (or another hardware decoder) said yes */
#define CAP_SW		2	/* only the browser's software decoder */
#define CAP_GPU		4	/* (caps[0] only) frames can be drawn into GPU resources: zero copy */

/* the status file (one per connection, written by the page): gpu/server/vdec.js has the same layout */
#define ST_MAGIC	0	/* "RVVS" */
#define ST_BEAT		4	/* page: +1 every turn */
#define ST_STATE	8	/* gpuserver: 1 open, 2 the page went away */
#define ST_CONSUMED	12	/* page: chunk bytes taken (mod 2^32) */
#define ST_SLOTS	4096
#define ST_SLOT_SIZE	32	/* decoded, ready, copied, copy_status, error, 3 spare */
#define ST_MAX_SLOTS	4096
#define ST_SIZE		(ST_SLOTS + ST_SLOT_SIZE * ST_MAX_SLOTS)

/* ---- objects */
#define ID_CONFIG	0x20000000
#define ID_CONTEXT	0x30000000
#define ID_BUFFER	0x40000000
#define ID_IMAGE	0x50000000
#define ID_MASK		0x0fffffff

struct vbuf {
	VABufferType type;
	uint32_t size, num;	/* bytes of one element, elements */
	uint8_t *data;
	int mem;		/* an image's buffer: gpuserver's memory file (data is its mapping) */
	int mem_fd;
	uint32_t mem_size;
	int mapped;
};

struct vconfig {
	VAProfile profile;
	VAEntrypoint entrypoint;
	int codec;
};

struct vsurface {
	int used;
	uint32_t width, height;	/* as created */
	uint32_t seq;		/* the chunk whose frame this surface is waiting for (0: none) */
	int pending;		/* a chunk was sent and its frame may not be there yet */
	/* zero copy: GPU resources of the render node (luma R8 w x h, chroma R8G8 w/2 x h/2) */
	uint32_t bo[2], res[2];
	int gpu;		/* every frame decoded into it is drawn into them */
};

struct vimage {
	int used;
	VAImage image;
};

struct buf {
	uint8_t *p;
	size_t n, cap;
};

/* What a packer makes of one picture: chunks in the buffer, in order. */
#define MAX_CHUNKS 4
struct chunks {
	struct buf b;
	int n;
	struct { size_t at, size; uint32_t flags, slot; } c[MAX_CHUNKS];	/* slot: the surface its frame is for (0: the target) */
};

struct vcontext;
struct codec {
	int codec;
	/* the picture's buffers (in vaRenderPicture order) into chunks; 0 or a VAStatus */
	VAStatus (*pack)(struct vcontext *c, struct vbuf **bufs, int n, struct chunks *out);
	void (*destroy)(struct vcontext *c);
	/* for VideoDecoderConfig.codec: profile and level the parameter sets say */
	void (*describe)(struct vcontext *c, uint32_t *profile, uint32_t *level);
};

struct vcontext {
	int used;
	uint32_t stream;
	struct vconfig cfg;
	const struct codec *codec;
	int width, height;
	VASurfaceID target;		/* between vaBeginPicture and vaEndPicture */
	struct vbuf **pic;		/* the picture's buffers */
	int npic, cappic;
	int started;			/* the stream was said to the page (with its first chunk) */
	uint32_t profile_said, level_said;
	void *state;			/* the codec's */
	struct driver *drv;
};

struct mem {
	int id;
	int fd;
	uint8_t *p;
	uint32_t size;
};

struct driver {
	pthread_mutex_t lock;		/* the objects */
	pthread_mutex_t sock_lock;	/* the socket */
	int sock;
	int drm_fd;			/* libva's, for GPU resources (zero copy) */
	uint8_t *status;		/* the status file */
	int status_fd;
	int ncaps;
	struct { uint32_t codec, profile, max_w, max_h, flags; } caps[32];
	int gpu;			/* zero copy possible */
	struct vconfig *configs; int nconfigs;
	struct vcontext *contexts; int ncontexts;
	struct vsurface *surfaces;	/* by VASurfaceID - 1 (the slot) */
	int nsurfaces;
	struct vbuf **buffers; int nbuffers;
	struct vimage *images; int nimages;
	uint32_t next_stream;
	uint32_t seq;
	/* chunk area: a ring in a memory file */
	struct mem chunk;
	pthread_mutex_t chunk_lock;	/* chunks go out one picture at a time, in decoding order */
	uint32_t written;		/* bytes put into it (mod 2^32), compared with the page's ST_CONSUMED */
	uint32_t chunk_base;		/* `written` when the area's offset 0 was where the next chunk goes */
	/* image buffers kept for reuse */
	struct mem keep[8]; int nkeep;
	uint32_t next_copy;
	int timeout_ms;
	int verbose;
	FILE *dump;			/* RVVDEC_DUMP: the chunks, as {u32 stream, u32 codec, u32 flags, u32 size} + bytes */
	struct {
		uint64_t chunks, keys, hidden, bytes, waits, wait_us, copies, copy_us, exports, errors, timeouts;
		uint32_t wait_max_us;
	} st;				/* said at the end (RVVDEC_STATS=0: not) */
};

/* conn.c */
int conn_open(struct driver *d);
void conn_close(struct driver *d);
int conn_cmd(struct driver *d, uint32_t cmd, const uint32_t *args, uint32_t n);
int conn_alloc(struct driver *d, uint32_t size, struct mem *m);
void conn_free(struct driver *d, struct mem *m);
uint32_t st_word(struct driver *d, uint32_t offset);
uint32_t *st_slot(struct driver *d, uint32_t slot);
/* Wait until `cond` holds (polled), the page goes away or the timeout passes: 0, or -1 */
int st_wait(struct driver *d, int (*cond)(struct driver *, void *), void *arg, int timeout_ms);
int seq_reached(uint32_t done, uint32_t target);

/* driver.c */
void vlog(struct driver *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define VLOG(d, ...) do { if ((d)->verbose) vlog(d, __VA_ARGS__); } while (0)
int buf_put(struct buf *b, const void *p, size_t n);
int buf_byte(struct buf *b, uint8_t v);
void chunk_begin(struct chunks *o);
void chunk_end(struct chunks *o, uint32_t flags);
struct vbuf *find_buf(struct vbuf **bufs, int n, VABufferType type, int index);
uint32_t surface_slot(struct driver *d, VASurfaceID id);

/* codecs */
extern const struct codec codec_h264, codec_hevc, codec_vp8, codec_vp9, codec_av1;

#endif
