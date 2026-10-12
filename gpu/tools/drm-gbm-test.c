/*
 * What Chromium's GPU process does with a render node, in small: open
 * /dev/dri/renderD128, make a GBM device on it, allocate a buffer, export it
 * as a dmabuf, import that into EGL (GBM platform, no surface) as an image,
 * draw into it with GL ES, and read it back two ways (glReadPixels and a CPU
 * mapping of the buffer through GBM). Built for riscv64 without the
 * libraries' headers (dlopen, the few declarations written out here):
 *   riscv64-linux-gnu-gcc -O2 -o drm-gbm-test drm-gbm-test.c -ldl
 *   drm-gbm-test [WIDTH HEIGHT FRAMES]
 * Prints what it sees and "ok" or "FAIL ..." per step; exit status 0 if all passed.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define GBM_FORMAT_XRGB8888 FOURCC('X', 'R', '2', '4')
#define GBM_FORMAT_ARGB8888 FOURCC('A', 'R', '2', '4')
#define GBM_BO_USE_SCANOUT 1
#define GBM_BO_USE_RENDERING 4
#define GBM_BO_USE_LINEAR 16
#define GBM_BO_TRANSFER_READ 1

typedef void *EGLDisplay, *EGLContext, *EGLImage, *EGLConfig, *EGLSurface;
typedef int32_t EGLint;
typedef intptr_t EGLAttrib;
#define EGL_PLATFORM_GBM_KHR 0x31D7
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_VENDOR 0x3053
#define EGL_EXTENSIONS 0x3055

#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SCISSOR_TEST 0x0C11

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); failures++; } printf("\n"); } while (0)

static void *sym(void *lib, const char *name)
{
	void *p = dlsym(lib, name);

	if (!p) {
		printf("FAIL no %s\n", name);
		exit(1);
	}
	return p;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int w = argc > 2 ? atoi(argv[1]) : 256, h = argc > 2 ? atoi(argv[2]) : 256, frames = argc > 3 ? atoi(argv[3]) : 60;
	void *gbm = dlopen("libgbm.so.1", RTLD_NOW), *egl = dlopen("libEGL.so.1", RTLD_NOW), *gles = dlopen("libGLESv2.so.2", RTLD_NOW);
	struct stat st;
	int fd;

	if (!gbm || !egl || !gles) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	void *(*gbm_create_device)(int) = sym(gbm, "gbm_create_device");
	const char *(*gbm_device_get_backend_name)(void *) = sym(gbm, "gbm_device_get_backend_name");
	void *(*gbm_bo_create)(void *, uint32_t, uint32_t, uint32_t, uint32_t) = sym(gbm, "gbm_bo_create");
	int (*gbm_bo_get_fd)(void *) = sym(gbm, "gbm_bo_get_fd");
	uint32_t (*gbm_bo_get_stride)(void *) = sym(gbm, "gbm_bo_get_stride");
	uint64_t (*gbm_bo_get_modifier)(void *) = sym(gbm, "gbm_bo_get_modifier");
	void *(*gbm_bo_map)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **) = sym(gbm, "gbm_bo_map");
	void (*gbm_bo_unmap)(void *, void *) = sym(gbm, "gbm_bo_unmap");
	void (*gbm_bo_destroy)(void *) = sym(gbm, "gbm_bo_destroy");
	EGLDisplay (*eglGetPlatformDisplay)(uint32_t, void *, const EGLAttrib *) = sym(egl, "eglGetPlatformDisplay");
	unsigned (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = sym(egl, "eglInitialize");
	unsigned (*eglBindAPI)(uint32_t) = sym(egl, "eglBindAPI");
	const char *(*eglQueryString)(EGLDisplay, EGLint) = sym(egl, "eglQueryString");
	EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = sym(egl, "eglCreateContext");
	unsigned (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = sym(egl, "eglMakeCurrent");
	EGLint (*eglGetError)(void) = sym(egl, "eglGetError");
	void *(*eglGetProcAddress)(const char *) = sym(egl, "eglGetProcAddress");
	EGLImage (*eglCreateImageKHR)(EGLDisplay, EGLContext, uint32_t, void *, const EGLint *) = eglGetProcAddress("eglCreateImageKHR");
	void (*glEGLImageTargetTexture2DOES)(uint32_t, EGLImage) = eglGetProcAddress("glEGLImageTargetTexture2DOES");
	const char *(*glGetString)(uint32_t) = sym(gles, "glGetString");
	void (*glGenTextures)(int, uint32_t *) = sym(gles, "glGenTextures");
	void (*glBindTexture)(uint32_t, uint32_t) = sym(gles, "glBindTexture");
	void (*glGenFramebuffers)(int, uint32_t *) = sym(gles, "glGenFramebuffers");
	void (*glBindFramebuffer)(uint32_t, uint32_t) = sym(gles, "glBindFramebuffer");
	void (*glFramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int) = sym(gles, "glFramebufferTexture2D");
	uint32_t (*glCheckFramebufferStatus)(uint32_t) = sym(gles, "glCheckFramebufferStatus");
	void (*glClearColor)(float, float, float, float) = sym(gles, "glClearColor");
	void (*glClear)(uint32_t) = sym(gles, "glClear");
	void (*glEnable)(uint32_t) = sym(gles, "glEnable");
	void (*glDisable)(uint32_t) = sym(gles, "glDisable");
	void (*glScissor)(int, int, int, int) = sym(gles, "glScissor");
	void (*glReadPixels)(int, int, int, int, uint32_t, uint32_t, void *) = sym(gles, "glReadPixels");
	void (*glFinish)(void) = sym(gles, "glFinish");
	void (*glFlush)(void) = sym(gles, "glFlush");

	fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0, "open /dev/dri/renderD128");
	if (fd < 0)
		return 1;
	CHECK(!fstat(fd, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 226 && minor(st.st_rdev) == 128,
	      "fstat: character device %u:%u", major(st.st_rdev), minor(st.st_rdev));
	void *dev = gbm_create_device(fd);
	CHECK(dev, "gbm_create_device: backend %s", dev ? gbm_device_get_backend_name(dev) : "-");
	if (!dev)
		return 1;
	void *bo = gbm_bo_create(dev, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
	CHECK(bo, "gbm_bo_create %dx%d XRGB8888", w, h);
	if (!bo)
		return 1;
	int dmabuf = gbm_bo_get_fd(bo);
	uint32_t stride = gbm_bo_get_stride(bo);
	CHECK(dmabuf >= 0, "gbm_bo_get_fd %d, stride %u, modifier 0x%llx", dmabuf, stride,
	      (unsigned long long)gbm_bo_get_modifier(bo));

	EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, dev, NULL);
	EGLint major_, minor_;
	CHECK(dpy && eglInitialize(dpy, &major_, &minor_), "EGL on GBM: %s", dpy ? eglQueryString(dpy, EGL_VENDOR) : "-");
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, NULL, NULL, cattr);
	CHECK(ctx && eglMakeCurrent(dpy, NULL, NULL, ctx), "context without config or surface (error 0x%x)", eglGetError());
	printf("     GL_RENDERER %s, GL_VERSION %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	const EGLint iattr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_XRGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_NONE };
	EGLImage img = eglCreateImageKHR ? eglCreateImageKHR(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, iattr) : NULL;
	CHECK(img, "EGLImage from the dmabuf (error 0x%x)", eglGetError());
	if (!img)
		return 1;
	uint32_t tex, fb;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer on the image");

	/* Red everywhere, a green square at the bottom left quarter. */
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, w / 2, h / 2);
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	uint8_t px[2][4];
	glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[0]);
	glReadPixels(w - 2, h - 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[1]);
	glFinish();
	CHECK(px[0][0] < 10 && px[0][1] > 245 && px[1][0] > 245 && px[1][1] < 10,
	      "glReadPixels: (1,1) = %u,%u,%u (green), (w-2,h-2) = %u,%u,%u (red)", px[0][0], px[0][1], px[0][2],
	      px[1][0], px[1][1], px[1][2]);

	/*
	 * The buffer's bytes, through a CPU mapping. Mesa's virgl reads a resource back from the host only if
	 * it drew into it itself (its clean_mask); the GBM buffer and the EGLImage are two resources of Mesa's
	 * on the same host resource, so the drawing above leaves the GBM one "clean" and its map would read
	 * the backing as it is. A whole-image glReadPixels fetches the backing first (TRANSFER_FROM_HOST into
	 * the file both map): what the map then shows is what the host wrote into the shared pages.
	 */
	uint8_t *all = malloc((size_t)w * h * 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, all);
	free(all);
	uint32_t mstride = 0;
	void *mdata = NULL;
	uint8_t *map = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &mstride, &mdata);
	CHECK(map, "gbm_bo_map (stride %u)", mstride);
	if (map) {
		/* XRGB8888 is B, G, R, X in memory. A texture's row 0 (GL's y = 0 when drawing into it) is the
		 * buffer's first row: (1,1) green, (w-2,h-2) red, as glReadPixels said. */
		const uint8_t *a = map + 1 * mstride + 4, *b = map + (h - 2) * mstride + 4 * (w - 2);
		CHECK(a[1] > 245 && a[2] < 10 && b[2] > 245 && b[1] < 10,
		      "the buffer's bytes: (1,1) B,G,R = %u,%u,%u (green), (w-2,h-2) = %u,%u,%u (red)",
		      a[0], a[1], a[2], b[0], b[1], b[2]);
		gbm_bo_unmap(bo, mdata);
	}

	/* Frames: a clear and a flush each, then a finish at the end (what a compositor's frame costs here at least). */
	double t0 = now();
	for (int i = 0; i < frames; i++) {
		glClearColor((i & 1) ? 1 : 0, 0, (i & 1) ? 0 : 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glFlush();
	}
	glFinish();
	double t = now() - t0;
	printf("     %d clear+flush frames in %.3f s: %.1f per second\n", frames, t, frames / t);

	gbm_bo_destroy(bo);
	close(dmabuf);
	printf("%s\n", failures ? "FAILED" : "all ok");
	return failures ? 1 : 0;
}
