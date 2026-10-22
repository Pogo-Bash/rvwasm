/*
 * vfront: the screen, contexts and sub-contexts, resources and their backings, transfers, and completion.
 * The command decoder is vf_decode.c; see vfront.h for the model.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vf_priv.h"

#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_debug.h"
#include "zink/zink_public.h"
#include <vulkan/vulkan_core.h>
#include "compiler/glsl_types.h"
#ifdef GALLIUM_SOFTPIPE
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"
#endif

struct vf_state vf;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Backings that are plain pointers of this process (the native vtest server maps gpuserver-like files). */
static void *plain_ptr(void *cookie, uint64_t addr, uint64_t size)
{
	(void)cookie;
	(void)size;
	return (void *)(uintptr_t)addr;
}

static void plain_read(void *cookie, uint64_t addr, void *dst, size_t size)
{
	(void)cookie;
	memcpy(dst, (const void *)(uintptr_t)addr, size);
}

static void plain_write(void *cookie, uint64_t addr, const void *src, size_t size)
{
	(void)cookie;
	memcpy((void *)(uintptr_t)addr, src, size);
}

static uint32_t debug_from_env(void)
{
	const char *e = getenv("VFRONT_DEBUG");
	uint32_t d = VF_DEBUG_ERROR;
	if (!e)
		return d;
	if (strstr(e, "cmd"))
		d |= VF_DEBUG_CMD;
	if (strstr(e, "shader"))
		d |= VF_DEBUG_SHADER;
	if (strstr(e, "sync"))
		d |= VF_DEBUG_SYNC;
	if (strstr(e, "caps"))
		d |= VF_DEBUG_CAPS;
	if (strstr(e, "quiet"))
		d &= ~VF_DEBUG_ERROR;
	return d;
}

/* zink asks vkGetInstanceProcAddr(NULL, ...) for what a Vulkan loader implements itself: an ICD reached
 * without a loader (wgvk in the page) may not have vkEnumerateInstanceLayerProperties, and zink then gives up
 * without a word. vfront answers for the loader: no layers, and Vulkan 1.0 when the driver has no
 * vkEnumerateInstanceVersion. */
static PFN_vkGetInstanceProcAddr driver_gipa;

static VKAPI_ATTR VkResult VKAPI_CALL
no_layers(uint32_t *count, VkLayerProperties *props)
{
	(void)props;
	*count = 0;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
version_1_0(uint32_t *version)
{
	*version = VK_API_VERSION_1_0;
	return VK_SUCCESS;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
loader_gipa(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction f = driver_gipa(instance, name);
	if (f)
		return f;
	if (!strcmp(name, "vkGetInstanceProcAddr"))
		return (PFN_vkVoidFunction)loader_gipa;
	if (!instance && !strcmp(name, "vkEnumerateInstanceLayerProperties"))
		return (PFN_vkVoidFunction)no_layers;
	if (!instance && !strcmp(name, "vkEnumerateInstanceVersion"))
		return (PFN_vkVoidFunction)version_1_0;
	return NULL;
}

int vf_init(const struct vf_init_args *args)
{
	if (vf.ready)
		return 0;
	memset(&vf, 0, sizeof(vf));
	vf.debug = debug_from_env() | (args ? args->debug : 0);
	if (args && args->mem) {
		vf.mem = *args->mem;
	} else {
		vf.mem.ptr = plain_ptr;
		vf.mem.read = plain_read;
		vf.mem.write = plain_write;
		vf.mem_plain = true;
	}
	if (args && args->vk_get_instance_proc_addr) {
		driver_gipa = (PFN_vkGetInstanceProcAddr)args->vk_get_instance_proc_addr;
		zink_set_vk_get_instance_proc_addr((void *)loader_gipa);
	}
	if (args && (args->flags & VF_INIT_SINGLE_THREAD)) {
		setenv("GALLIUM_THREAD", "0", 1);	/* (zink's flush thread) */
		/* zink's shader/pipeline compile queues run their jobs inline (patches/mesa-page.diff, u_queue.c) */
		setenv("MESA_UTIL_QUEUE_INLINE", "1", 1);
		/* (the disk cache's writer is not safe run inline, and the page has no disk) */
		setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
	}

	/* (NIR's type cache: tgsi_to_nir and the variant passes use it whatever the driver) */
	glsl_type_singleton_init_or_ref();
	const char *drv = getenv("VFRONT_DRIVER");
#ifdef GALLIUM_SOFTPIPE
	if (drv && !strcmp(drv, "softpipe"))
		vf.screen = softpipe_create_screen(null_sw_create());
	else
#endif
	if (drv && strcmp(drv, "zink"))
		vf_log("VFRONT_DRIVER=%s is not built in; zink\n", drv);
	if (!vf.screen) {
		vf.screen = zink_create_screen(NULL, NULL);
		vf.image_to_buffer = true;
	}
	if (!vf.screen) {
		vf_log("zink found no usable Vulkan device\n");
		return -ENODEV;
	}
	vf.resources = _mesa_hash_table_create_u32_keys(NULL);
	snprintf(vf.renderer, sizeof(vf.renderer), "%s", vf.screen->get_name(vf.screen));
	vf_caps_fill(&vf.caps);
	vf.ready = true;
	return 0;
}

void vf_fini(void)
{
	if (!vf.ready)
		return;
	for (unsigned i = 0; i < ARRAY_SIZE(vf.ctx); i++)
		if (vf.ctx[i])
			vf_ctx_destroy(i);
	hash_table_foreach(vf.resources, he) {
		struct vf_resource *r = he->data;
		pipe_resource_reference(&r->pres, NULL);
		free(r);
	}
	_mesa_hash_table_destroy(vf.resources, NULL);
	vf.screen->destroy(vf.screen);
	glsl_type_singleton_decref();
	vf.ready = false;
}

uint32_t vf_caps(void *out, uint32_t max_size)
{
	if (max_size < sizeof(vf.caps.v2))
		return 0;
	memcpy(out, &vf.caps.v2, sizeof(vf.caps.v2));
	return sizeof(vf.caps.v2);
}

void vf_set_guest_caps(const void *caps, uint32_t size)
{
	memcpy(&vf.caps.v2, caps, MIN2(size, sizeof(vf.caps.v2)));
}

const char *vf_renderer_name(void)
{
	return vf.renderer;
}

void vf_get_stats(struct vf_stats *stats)
{
	*stats = vf.stats;
}

/* ---- resources ---- */

struct vf_resource *vf_res_lookup(uint32_t id)
{
	struct hash_entry *he = _mesa_hash_table_search(vf.resources, (void *)(uintptr_t)id);
	return he ? he->data : NULL;
}

struct pipe_resource *vf_pres(uint32_t id)
{
	struct vf_resource *r = id ? vf_res_lookup(id) : NULL;
	return r ? r->pres : NULL;
}

struct pipe_resource *vf_res_pipe(uint32_t id)
{
	return vf_pres(id);
}

void vf_flush_pending(void)
{
	/* (only one pipe has unflushed work at a time: vf.last_pipe; a flush with the context's last serial
	 * again, since what is unflushed then is uploads, which complete without one) */
	if (!vf.last_pipe)
		return;
	for (unsigned i = 0; i < ARRAY_SIZE(vf.ctx); i++) {
		struct vf_ctx *c = vf.ctx[i];
		if (c && c->last_pipe == vf.last_pipe) {
			vf_ctx_flush(c->id, c->flushed_serial);
			return;
		}
	}
}

static unsigned pipe_bind(uint32_t vbind)
{
	static const struct {
		uint32_t v, p;
	} map[] = {
		{ VIRGL_BIND_DEPTH_STENCIL, PIPE_BIND_DEPTH_STENCIL },
		{ VIRGL_BIND_RENDER_TARGET, PIPE_BIND_RENDER_TARGET },
		{ VIRGL_BIND_SAMPLER_VIEW, PIPE_BIND_SAMPLER_VIEW },
		{ VIRGL_BIND_VERTEX_BUFFER, PIPE_BIND_VERTEX_BUFFER },
		{ VIRGL_BIND_INDEX_BUFFER, PIPE_BIND_INDEX_BUFFER },
		{ VIRGL_BIND_CONSTANT_BUFFER, PIPE_BIND_CONSTANT_BUFFER },
		{ VIRGL_BIND_DISPLAY_TARGET, PIPE_BIND_DISPLAY_TARGET },
		{ VIRGL_BIND_COMMAND_ARGS, PIPE_BIND_COMMAND_ARGS_BUFFER },
		{ VIRGL_BIND_STREAM_OUTPUT, PIPE_BIND_STREAM_OUTPUT },
		{ VIRGL_BIND_SHADER_BUFFER, PIPE_BIND_SHADER_BUFFER },
		{ VIRGL_BIND_QUERY_BUFFER, PIPE_BIND_QUERY_BUFFER },
		{ VIRGL_BIND_CURSOR, PIPE_BIND_CURSOR },
		{ VIRGL_BIND_SCANOUT, PIPE_BIND_SCANOUT },
		{ VIRGL_BIND_SHARED, PIPE_BIND_SHARED },
		{ VIRGL_BIND_LINEAR, PIPE_BIND_LINEAR },
	};
	unsigned b = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(map); i++)
		if (vbind & map[i].v)
			b |= map[i].p;
	return b;
}

int vf_res_create(uint32_t ctx, const struct vf_res_create_args *a, uint64_t backing, uint64_t backing_size)
{
	if (!vf.ready)
		return -ENODEV;
	if (!a->res || vf_res_lookup(a->res)) {
		vf_err("resource %u: bad or already there\n", a->res);
		return -EINVAL;
	}
	struct vf_resource *r = calloc(1, sizeof(*r));
	r->id = a->res;
	r->ctx = ctx;
	r->backing = backing;
	r->backing_size = backing_size;
	r->virgl_format = a->format;
	r->bind = a->bind;
	r->flags = a->flags;

	/* Fences, query results and staging buffers exist only in their backing: nothing on the GPU reads them
	 * (vrend keeps them in host memory too). */
	bool gpu = !(a->bind & (VIRGL_BIND_CUSTOM | VIRGL_BIND_STAGING));
	if (a->target == PIPE_BUFFER && a->width == 0)
		gpu = false;

	if (gpu) {
		struct pipe_resource t;
		memset(&t, 0, sizeof(t));
		t.target = a->target;
		t.format = a->target == PIPE_BUFFER ? PIPE_FORMAT_R8_UNORM : vf_format(a->format);
		t.width0 = a->width;
		t.height0 = MAX2(a->height, 1);
		t.depth0 = MAX2(a->depth, 1);
		t.array_size = MAX2(a->array_size, 1);
		t.last_level = a->last_level;
		t.nr_samples = a->nr_samples > 1 ? a->nr_samples : 0;
		t.nr_storage_samples = t.nr_samples;
		/* nothing is shown through a winsys here (frames reach the page another way), so no display
		 * target or scanout binds: drivers would ask a winsys for those */
		t.bind = pipe_bind(a->bind) & ~(PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED);
		t.usage = PIPE_USAGE_DEFAULT;
		if (a->target != PIPE_BUFFER && t.format == PIPE_FORMAT_NONE) {
			vf_err("resource %u: unknown virgl format %u\n", a->res, a->format);
			free(r);
			return -EINVAL;
		}
		/* Textures may become render targets or images later without saying so (virgl's binds are hints). */
		if (a->target != PIPE_BUFFER) {
			const struct util_format_description *d = util_format_description(t.format);
			if (d && d->colorspace == UTIL_FORMAT_COLORSPACE_ZS)
				t.bind |= PIPE_BIND_DEPTH_STENCIL;
			t.bind |= PIPE_BIND_SAMPLER_VIEW;
		}
		/* virgl's caps say "multisampled up to max_samples" per format, so the guest asks for counts
		 * the driver may not have (lavapipe: 1 and 4 only); GL allows more samples than asked for. */
		if (t.nr_samples > 1) {
			unsigned check = t.bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SAMPLER_VIEW);
			unsigned n = t.nr_samples;
			while (n <= 32 && !vf.screen->is_format_supported(vf.screen, t.format, t.target, n, n, check))
				n++;
			if (n <= 32) {
				if (n != t.nr_samples && (vf.debug & VF_DEBUG_CMD))
					vf_err("resource %u: %u samples instead of %u\n", a->res, n, t.nr_samples);
				t.nr_samples = t.nr_storage_samples = n;
			}
		}
		r->pres = vf.screen->resource_create(vf.screen, &t);
		if (!r->pres && (t.bind & ~(PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_DEPTH_STENCIL))) {
			/* retry with only what the guest asked for */
			t.bind = pipe_bind(a->bind) & ~(PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED);
			r->pres = vf.screen->resource_create(vf.screen, &t);
		}
		if (!r->pres) {
			vf_err("resource %u: zink could not make target %u format %s %ux%ux%u layers %u levels %u "
			       "samples %u bind 0x%x\n", a->res, a->target, util_format_name(t.format), t.width0,
			       t.height0, t.depth0, t.array_size, t.last_level + 1, t.nr_samples, t.bind);
			free(r);
			return -ENOMEM;
		}
	}
	_mesa_hash_table_insert(vf.resources, (void *)(uintptr_t)r->id, r);
	return 0;
}

void vf_res_unref(uint32_t ctx, uint32_t res)
{
	(void)ctx;
	struct hash_entry *he = _mesa_hash_table_search(vf.resources, (void *)(uintptr_t)res);
	if (!he)
		return;
	struct vf_resource *r = he->data;
	_mesa_hash_table_remove(vf.resources, he);
	/* objects and pending work hold their own references to the pipe_resource */
	pipe_resource_reference(&r->pres, NULL);
	free(r);
}

/* A pointer to [offset, offset + size) of r's backing: directly when the memory allows it, else a copy in
 * *tmp (read in unless `fill` is false: the caller writes all of it). NULL when the range is outside the
 * backing. */
void *vf_backing_ptr2(struct vf_resource *r, uint64_t offset, uint64_t size, void **tmp, bool fill)
{
	*tmp = NULL;
	if (!r->backing || offset > r->backing_size || size > r->backing_size - offset) {
		vf_err("resource %u: range %" PRIu64 "+%" PRIu64 " is outside its backing (%" PRIu64 " bytes)\n",
		       r->id, offset, size, r->backing_size);
		return NULL;
	}
	if (vf.mem.ptr) {
		void *p = vf.mem.ptr(vf.mem.cookie, r->backing + offset, size);
		if (p)
			return p;
	}
	*tmp = malloc(size ? size : 1);
	if (fill)
		vf.mem.read(vf.mem.cookie, r->backing + offset, *tmp, size);
	return *tmp;
}

void *vf_backing_ptr(struct vf_resource *r, uint64_t offset, uint64_t size, void **tmp)
{
	return vf_backing_ptr2(r, offset, size, tmp, true);
}

void vf_backing_done(struct vf_resource *r, uint64_t offset, uint64_t size, void *tmp, bool written)
{
	if (!tmp)
		return;
	if (written)
		vf.mem.write(vf.mem.cookie, r->backing + offset, tmp, size);
	free(tmp);
}

/* GPU work goes out in the order the stream has it, whichever pipe_context it was given to: when the next
 * piece of work is for another context, the previous one is flushed first (all zink contexts share one
 * queue). */
void vf_use_pipe(struct vf_ctx *c, struct pipe_context *pipe)
{
	if (vf.last_pipe && vf.last_pipe != pipe)
		vf.last_pipe->flush(vf.last_pipe, NULL, 0);
	vf.last_pipe = pipe;
	if (c)
		c->last_pipe = pipe;
}

/* ---- contexts ---- */

static struct vf_ctx *ctx_get(uint32_t id)
{
	return id < ARRAY_SIZE(vf.ctx) ? vf.ctx[id] : NULL;
}

int vf_ctx_create(uint32_t id, const char *name)
{
	if (!vf.ready)
		return -ENODEV;
	if (!id || id >= ARRAY_SIZE(vf.ctx) || vf.ctx[id])
		return -EINVAL;
	struct vf_ctx *c = calloc(1, sizeof(*c));
	c->id = id;
	snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
	c->subs = _mesa_hash_table_create_u32_keys(NULL);
	util_dynarray_init(&c->readbacks, NULL);
	util_dynarray_init(&c->queries, NULL);
	util_dynarray_init(&c->pending, NULL);
	vf.ctx[id] = c;
	return 0;
}

/* Sub-context ids start at 0, which a hash table key cannot be: they are stored as id + 1. */
struct vf_sub *vf_sub_get(struct vf_ctx *c, uint32_t id, bool create)
{
	struct hash_entry *he = _mesa_hash_table_search(c->subs, (void *)(uintptr_t)(id + 1));
	if (he)
		return he->data;
	if (!create)
		return NULL;
	struct pipe_context *pipe = vf.screen->context_create(vf.screen, NULL, 0);
	if (!pipe) {
		vf_err("context %u: zink could not make a pipe_context for sub-context %u\n", c->id, id);
		return NULL;
	}
	struct vf_sub *s = calloc(1, sizeof(*s));
	s->id = id;
	s->ctx = c;
	s->pipe = pipe;
	s->objects = _mesa_hash_table_create_u32_keys(NULL);
	pipe->set_sample_mask(pipe, ~0u);
	if (pipe->set_min_samples)
		pipe->set_min_samples(pipe, 1);
	_mesa_hash_table_insert(c->subs, (void *)(uintptr_t)(id + 1), s);
	return s;
}

void vf_sub_destroy(struct vf_sub *s)
{
	struct vf_ctx *c = s->ctx;
	/* what is still pending may use this pipe_context */
	vf_ctx_poll(c->id, UINT64_MAX);
	hash_table_foreach(s->objects, he)
		vf_object_destroy(s, he->data);
	_mesa_hash_table_destroy(s->objects, NULL);
	for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
		if (s->long_shader[i]) {
			free(s->long_shader[i]->text);
			free(s->long_shader[i]);
		}
	}
	for (unsigned i = 0; i < s->num_so_targets; i++)
		pipe_so_target_reference(&s->so_targets[i], NULL);
	if (vf.last_pipe == s->pipe)
		vf.last_pipe = NULL;
	if (c->last_pipe == s->pipe)
		c->last_pipe = NULL;
	s->pipe->destroy(s->pipe);
	for (unsigned st = 0; st < MESA_SHADER_STAGES; st++) {
		for (unsigned i = 0; i < PIPE_MAX_SHADER_IMAGES; i++)
			pipe_resource_reference(&s->image_res[st][i], NULL);
		for (unsigned i = 0; i < PIPE_MAX_SHADER_BUFFERS; i++)
			pipe_resource_reference(&s->ssbo_res[st][i], NULL);
	}
	for (unsigned i = 0; i < PIPE_MAX_HW_ATOMIC_BUFFERS; i++)
		pipe_resource_reference(&s->atomic_res[i], NULL);
	struct hash_entry *he = _mesa_hash_table_search(c->subs, (void *)(uintptr_t)(s->id + 1));
	if (he)
		_mesa_hash_table_remove(c->subs, he);
	if (c->sub == s)
		c->sub = NULL;
	free(s);
}

void vf_ctx_destroy(uint32_t id)
{
	struct vf_ctx *c = ctx_get(id);
	if (!c)
		return;
	vf_ctx_flush(id, c->flushed_serial);
	vf_ctx_poll(id, UINT64_MAX);
	hash_table_foreach(c->subs, he)
		vf_sub_destroy(he->data);
	_mesa_hash_table_destroy(c->subs, NULL);
	util_dynarray_fini(&c->readbacks);
	util_dynarray_fini(&c->queries);
	util_dynarray_fini(&c->pending);
	vf.ctx[id] = NULL;
	free(c);
}

/* The sub-context transfers go to: the current one, or sub-context 0 (which vrend makes for every context). */
static struct vf_sub *ctx_sub(struct vf_ctx *c)
{
	if (!c->sub)
		c->sub = vf_sub_get(c, 0, true);
	return c->sub;
}

int vf_submit(uint32_t id, const uint32_t *dw, uint32_t ndw)
{
	struct vf_ctx *c = ctx_get(id);
	if (!c)
		return -EINVAL;
	vf.stats.submits++;
	int ret = vf_decode(c, dw, ndw);
	if (vf.debug & VF_DEBUG_SYNC) {
		vf_ctx_flush(id, c->flushed_serial);
		vf_ctx_poll(id, UINT64_MAX);
	}
	return ret;
}

/* ---- transfers ---- */

static void box_to_pipe(const struct vf_box *b, struct pipe_box *p)
{
	u_box_3d(b->x, b->y, b->z, b->w, b->h, b->d, p);
}

/* The size a box takes in memory with these strides. */
static uint64_t box_bytes(enum pipe_format f, const struct vf_box *b, uint32_t stride, uint32_t layer_stride)
{
	if (!b->w || !b->h || !b->d)
		return 0;
	uint32_t rows = util_format_get_nblocksy(f, b->h);
	return (uint64_t)layer_stride * (b->d - 1) + (uint64_t)stride * (rows - 1) + util_format_get_stride(f, b->w);
}

static void natural_strides(struct pipe_resource *p, uint32_t level, uint32_t *stride, uint32_t *layer_stride)
{
	if (!*stride)
		*stride = util_format_get_stride(p->format, u_minify(p->width0, level));
	if (!*layer_stride)
		*layer_stride = util_format_get_2d_size(p->format, *stride, u_minify(p->height0, level));
}

/* Data for `dst` from the backing of `src` (dst itself, or a staging resource) at `offset`. */
int vf_put_from_backing(struct vf_sub *s, struct vf_resource *dst, uint32_t level, const struct vf_box *box,
			uint32_t stride, uint32_t layer_stride, uint64_t offset, struct vf_resource *src)
{
	struct pipe_resource *p = dst->pres;
	if (!p)
		return 0;	/* only a backing: it already is the data */
	if (!src)
		src = dst;
	struct pipe_context *pipe = s->pipe;
	vf_use_pipe(s->ctx, pipe);
	void *tmp;
	if (p->target == PIPE_BUFFER) {
		if ((uint64_t)box->x + box->w > p->width0)
			return -EINVAL;
		const void *data = vf_backing_ptr(src, offset, box->w, &tmp);
		if (!data)
			return -EINVAL;
		pipe->buffer_subdata(pipe, p, PIPE_MAP_WRITE, box->x, box->w, data);
		vf_backing_done(src, offset, box->w, tmp, false);
		vf.stats.put_bytes += box->w;
		return 0;
	}
	natural_strides(p, level, &stride, &layer_stride);
	uint64_t size = box_bytes(p->format, box, stride, layer_stride);
	const void *data = vf_backing_ptr(src, offset, size, &tmp);
	if (!data)
		return -EINVAL;
	struct pipe_box pb;
	box_to_pipe(box, &pb);
	pipe->texture_subdata(pipe, p, level, PIPE_MAP_WRITE, &pb, data, stride, layer_stride);
	vf_backing_done(src, offset, size, tmp, false);
	vf.stats.put_bytes += size;
	return 0;
}

int vf_transfer_put(uint32_t id, uint32_t res, uint32_t level, const struct vf_box *box, uint64_t offset)
{
	struct vf_ctx *c = ctx_get(id);
	struct vf_resource *r = vf_res_lookup(res);
	if (!c || !r)
		return -EINVAL;
	struct vf_sub *s = ctx_sub(c);
	if (!s)
		return -ENOMEM;
	vf.stats.puts++;
	return vf_put_from_backing(s, r, level, box, 0, 0, offset, r);
}

/* A readback: copy the box into a staging resource now; the copy is read into the backing once the flush
 * it belongs to has completed (vf_ctx_poll), so nothing here waits for the GPU. */
int vf_get_to_backing(struct vf_sub *s, struct vf_resource *r, uint32_t level, const struct vf_box *box,
		      uint32_t stride, uint32_t layer_stride, uint64_t offset, struct vf_resource *dst)
{
	struct pipe_resource *p = r->pres;
	if (!p)
		return 0;
	if (!dst)
		dst = r;
	struct pipe_context *pipe = s->pipe;
	vf_use_pipe(s->ctx, pipe);

	struct vf_readback rb;
	memset(&rb, 0, sizeof(rb));
	rb.res = dst->id;
	rb.level = level;
	rb.box = *box;
	rb.offset = offset;

	struct pipe_box src_box;
	box_to_pipe(box, &src_box);
	rb.format = p->format;
	const struct util_format_description *desc = util_format_description(p->format);
	bool zs = desc && desc->colorspace == UTIL_FORMAT_COLORSPACE_ZS;
	struct pipe_resource t;
	memset(&t, 0, sizeof(t));
	t.usage = PIPE_USAGE_STAGING;
	t.height0 = 1;
	t.depth0 = 1;
	t.array_size = 1;
	if (p->target == PIPE_BUFFER) {
		t.target = PIPE_BUFFER;
		t.format = PIPE_FORMAT_R8_UNORM;
		t.width0 = box->w;
	} else if (vf.image_to_buffer && !zs && p->nr_samples <= 1 && p->target != PIPE_TEXTURE_1D_ARRAY) {
		/* zink copies an image into a buffer (rows packed); a buffer maps without waiting for anything
		 * more, so the readback finishes without another round trip to the GPU */
		t.target = PIPE_BUFFER;
		t.format = PIPE_FORMAT_R8_UNORM;
		t.width0 = util_format_get_stride(p->format, box->w) * util_format_get_nblocksy(p->format, box->h) *
			   box->d;
		rb.packed = true;
	} else {
		/* (depth/stencil, multisampled, 1D arrays: a staging texture, mapped with a wait at the end) */
		t.format = p->format;
		t.width0 = box->w;
		switch (p->target) {
		case PIPE_TEXTURE_1D:
			t.target = PIPE_TEXTURE_1D;
			break;
		case PIPE_TEXTURE_1D_ARRAY:
			t.target = box->h > 1 ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
			t.array_size = box->h;
			break;
		case PIPE_TEXTURE_3D:
			t.target = PIPE_TEXTURE_3D;
			t.height0 = box->h;
			t.depth0 = box->d;
			break;
		default:
			t.target = box->d > 1 ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
			t.height0 = box->h;
			t.array_size = box->d;
			break;
		}
	}
	rb.staging = vf.screen->resource_create(vf.screen, &t);
	if (!rb.staging) {
		vf_err("resource %u: no staging %s %ux%u for a readback\n", r->id, util_format_name(p->format),
		       box->w, box->h);
		return -ENOMEM;
	}
	if (p->nr_samples > 1) {
		/* a multisampled resource is resolved into the staging copy */
		struct pipe_blit_info bi;
		memset(&bi, 0, sizeof(bi));
		bi.src.resource = p;
		bi.src.level = level;
		bi.src.box = src_box;
		bi.src.format = p->format;
		bi.dst.resource = rb.staging;
		u_box_3d(0, 0, 0, box->w, box->h, box->d, &bi.dst.box);
		bi.dst.format = p->format;
		bi.mask = util_format_get_mask(p->format);
		bi.filter = PIPE_TEX_FILTER_NEAREST;
		pipe->blit(pipe, &bi);
	} else {
		pipe->resource_copy_region(pipe, rb.staging, 0, 0, 0, 0, p, level, &src_box);
	}
	/* the strides the backing wants (natural ones of the level unless the command said otherwise) */
	if (p->target != PIPE_BUFFER)
		natural_strides(p, level, &stride, &layer_stride);
	rb.stride = stride;
	rb.layer_stride = layer_stride;
	util_dynarray_append_typed(&s->ctx->readbacks, struct vf_readback, rb);
	return 0;
}

int vf_transfer_get(uint32_t id, uint32_t res, uint32_t level, const struct vf_box *box, uint64_t offset)
{
	struct vf_ctx *c = ctx_get(id);
	struct vf_resource *r = vf_res_lookup(res);
	if (!c || !r)
		return -EINVAL;
	struct vf_sub *s = ctx_sub(c);
	if (!s)
		return -ENOMEM;
	vf.stats.gets++;
	return vf_get_to_backing(s, r, level, box, 0, 0, offset, r);
}

/* Read a finished staging copy into the backing. */
static void readback_finish(struct vf_readback *rb, struct pipe_context *pipe)
{
	struct vf_resource *dst = vf_res_lookup(rb->res);
	struct pipe_resource *st = rb->staging;
	uint32_t stride = rb->stride, layer_stride = rb->layer_stride;
	struct pipe_transfer *xfer = NULL;
	if (!dst || !pipe)
		goto out;
	if (rb->packed) {
		/* rows packed one after the other in a buffer: into the backing with its strides */
		enum pipe_format f = rb->format;
		const uint8_t *m = pipe_buffer_map_range(pipe, st, 0, st->width0, PIPE_MAP_READ | PIPE_MAP_UNSYNCHRONIZED,
							 &xfer);
		if (!m)
			goto out;
		struct vf_box vb = { 0, 0, 0, rb->box.w, rb->box.h, rb->box.d };
		uint64_t size = box_bytes(f, &vb, stride, layer_stride);
		unsigned rows = util_format_get_nblocksy(f, vb.h);
		unsigned row_bytes = util_format_get_stride(f, vb.w);
		void *tmp;
		bool contiguous = stride == row_bytes && (vb.d == 1 || layer_stride == row_bytes * rows);
		uint8_t *out = vf_backing_ptr2(dst, rb->offset, size, &tmp, !contiguous);
		if (out) {
			if (contiguous) {
				memcpy(out, m, size);
			} else {
				for (unsigned z = 0; z < vb.d; z++)
					for (unsigned y = 0; y < rows; y++)
						memcpy(out + (uint64_t)z * layer_stride + (uint64_t)y * stride,
						       m + ((uint64_t)z * rows + y) * row_bytes, row_bytes);
			}
			vf_backing_done(dst, rb->offset, size, tmp, true);
		}
		pipe_buffer_unmap(pipe, xfer);
		vf.stats.get_bytes += size;
		goto out;
	}
	if (st->target == PIPE_BUFFER) {
		const void *m = pipe_buffer_map_range(pipe, st, 0, rb->box.w, PIPE_MAP_READ | PIPE_MAP_UNSYNCHRONIZED,
						       &xfer);
		if (!m)
			goto out;
		void *tmp;
		void *out = vf_backing_ptr2(dst, rb->offset, rb->box.w, &tmp, false);
		if (out) {
			memcpy(out, m, rb->box.w);
			vf_backing_done(dst, rb->offset, rb->box.w, tmp, true);
		}
		pipe_buffer_unmap(pipe, xfer);
		vf.stats.get_bytes += rb->box.w;
		goto out;
	}
	struct pipe_box b;
	unsigned layers = st->target == PIPE_TEXTURE_3D ? st->depth0 : st->array_size;
	unsigned height = st->target == PIPE_TEXTURE_1D_ARRAY ? 1 : st->height0;
	u_box_3d(0, 0, 0, st->width0, st->target == PIPE_TEXTURE_1D_ARRAY ? st->array_size : height,
		 st->target == PIPE_TEXTURE_1D_ARRAY ? 1 : layers, &b);
	/* (zink copies an optimally tiled image out to map it, so this one waits) */
	const uint8_t *m = pipe->texture_map(pipe, st, 0, PIPE_MAP_READ, &b, &xfer);
	if (!m)
		goto out;
	enum pipe_format f = st->format;
	struct vf_box vb = { 0, 0, 0, rb->box.w, rb->box.h, rb->box.d };
	uint64_t size = box_bytes(f, &vb, stride, layer_stride);
	void *tmp;
	uint8_t *out = vf_backing_ptr(dst, rb->offset, size, &tmp);
	if (out) {
		unsigned rows = util_format_get_nblocksy(f, vb.h);
		unsigned row_bytes = util_format_get_stride(f, vb.w);
		for (unsigned z = 0; z < vb.d; z++)
			for (unsigned y = 0; y < rows; y++)
				memcpy(out + (uint64_t)z * layer_stride + (uint64_t)y * stride,
				       m + (uint64_t)z * xfer->layer_stride + (uint64_t)y * xfer->stride, row_bytes);
		vf_backing_done(dst, rb->offset, size, tmp, true);
	}
	pipe->texture_unmap(pipe, xfer);
	vf.stats.get_bytes += size;
out:
	pipe_resource_reference(&rb->staging, NULL);
}

/* A query result into the backing of its resource, as struct virgl_host_query_state. */
static void query_finish(struct vf_query_write *qw)
{
	union pipe_query_result result;
	memset(&result, 0, sizeof(result));
	struct virgl_host_query_state st;
	memset(&st, 0, sizeof(st));
	if (!qw->pipe->get_query_result(qw->pipe, qw->q, true, &result))
		return;
	switch (qw->type) {
	case PIPE_QUERY_OCCLUSION_PREDICATE:
	case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
	case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
	case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
	case PIPE_QUERY_GPU_FINISHED:
		st.result = result.b;
		st.result_size = 4;
		break;
	case PIPE_QUERY_SO_STATISTICS:
		st.result = result.so_statistics.num_primitives_written;
		st.result_size = 8;
		break;
	case PIPE_QUERY_TIMESTAMP_DISJOINT:
		st.result = result.timestamp_disjoint.frequency;
		st.result_size = 8;
		break;
	default:
		st.result = result.u64;
		st.result_size = (qw->type == PIPE_QUERY_TIMESTAMP || qw->type == PIPE_QUERY_TIME_ELAPSED) ? 8 : 4;
		break;
	}
	st.query_state = VIRGL_QUERY_STATE_DONE;
	struct vf_resource *r = vf_res_lookup(qw->res);
	if (!r)
		return;
	void *tmp;
	void *out = vf_backing_ptr2(r, qw->offset, sizeof(st), &tmp, false);
	if (out) {
		memcpy(out, &st, sizeof(st));
		vf_backing_done(r, qw->offset, sizeof(st), tmp, true);
	}
}


int vf_ctx_flush(uint32_t id, uint32_t serial)
{
	struct vf_ctx *c = ctx_get(id);
	if (!c)
		return -EINVAL;
	struct vf_pending p;
	memset(&p, 0, sizeof(p));
	p.serial = serial;
	if (c->last_pipe) {
		c->last_pipe->flush(c->last_pipe, &p.fence, 0);
		if (vf.last_pipe == c->last_pipe)
			vf.last_pipe = NULL;
		vf.stats.flushes++;
	}
	/* hand over what completion has to do */
	p.readbacks = c->readbacks;
	p.queries = c->queries;
	util_dynarray_init(&c->readbacks, NULL);
	util_dynarray_init(&c->queries, NULL);
	bool effects = util_dynarray_num_elements(&p.readbacks, struct vf_readback) ||
		       util_dynarray_num_elements(&p.queries, struct vf_query_write);
	util_dynarray_append_typed(&c->pending, struct vf_pending, p);
	c->flushed_serial = serial;
	return effects ? 1 : 0;
}

uint32_t vf_ctx_poll(uint32_t id, uint64_t timeout_ns)
{
	struct vf_ctx *c = ctx_get(id);
	if (!c)
		return 0;
	unsigned n = util_dynarray_num_elements(&c->pending, struct vf_pending);
	unsigned done = 0;
	uint64_t deadline = timeout_ns == UINT64_MAX ? UINT64_MAX : now_ns() + timeout_ns;
	struct vf_pending *ps = util_dynarray_begin(&c->pending);
	for (; done < n; done++) {
		struct vf_pending *p = &ps[done];
		if (p->fence) {
			uint64_t left = 0;
			if (deadline == UINT64_MAX)
				left = OS_TIMEOUT_INFINITE;
			else {
				uint64_t t = now_ns();
				left = t < deadline ? deadline - t : 0;
			}
			if (!vf.screen->fence_finish(vf.screen, NULL, p->fence, left))
				break;
			vf.screen->fence_reference(vf.screen, &p->fence, NULL);
		}
		util_dynarray_foreach(&p->readbacks, struct vf_readback, rb)
			readback_finish(rb, c->sub ? c->sub->pipe : NULL);
		util_dynarray_foreach(&p->queries, struct vf_query_write, qw) {
			query_finish(qw);
			vf_query_unref(qw->query, qw->pipe);
		}
		util_dynarray_fini(&p->readbacks);
		util_dynarray_fini(&p->queries);
		c->done_serial = p->serial;
	}
	if (done) {
		unsigned left = n - done;
		memmove(ps, ps + done, left * sizeof(*ps));
		c->pending.size = left * sizeof(*ps);
	}
	return c->done_serial;
}
