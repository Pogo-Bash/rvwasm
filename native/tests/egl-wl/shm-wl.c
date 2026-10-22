/*
 * VK-18 bisect: the wl_shm buffer path of Mesa's EGL wayland swrast (dri2_wl_swrast_allocate_buffer / put_image2 /
 * commit_backbuffer) without Mesa: a file of W*H*4 bytes (memfd or a file in XDG_RUNTIME_DIR), mmap MAP_SHARED,
 * a pool + buffer, FRAMES frames of fill + attach + damage + commit, waiting for the frame callback.
 *   shm-wl [FRAMES [W [H]]]    env: SHM_FILE=1 (mkstemp in XDG_RUNTIME_DIR instead of memfd), SHM_NOFILL=1
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static int configured, frame_done;

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) { xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };
static void surf_configure(void *d, struct xdg_surface *s, uint32_t serial) { xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener surf_listener = { surf_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {}
static void top_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close };
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
	else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "xdg_wm_base")) { wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm_base, &wm_listener, NULL); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };
static void frame_cb(void *d, struct wl_callback *cb, uint32_t t) { frame_done = 1; wl_callback_destroy(cb); }
static const struct wl_callback_listener frame_listener = { frame_cb };

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 100, W = argc > 2 ? atoi(argv[2]) : 420, H = argc > 3 ? atoi(argv[3]) : 320;
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { printf("shm-wl: no display\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	struct wl_surface *surf = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
	xdg_surface_add_listener(xs, &surf_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &top_listener, NULL);
	xdg_toplevel_set_title(top, "shm-wl");
	wl_surface_commit(surf);
	while (!configured) wl_display_dispatch(dpy);

	int stride = W * 4;
	size_t size = (size_t)stride * H;
	int fd;
	if (getenv("SHM_FILE")) {
		char name[256];
		snprintf(name, sizeof name, "%s/shm-wl-XXXXXX", getenv("XDG_RUNTIME_DIR"));
		fd = mkostemp(name, O_CLOEXEC);
		unlink(name);
	} else
		fd = memfd_create("shm-wl", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0 || ftruncate(fd, size) < 0) { perror("shm-wl: file"); return 1; }
	unsigned char *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) { perror("shm-wl: mmap"); return 1; }
	printf("shm-wl: %s fd %d size %zu mapped at %p\n", getenv("SHM_FILE") ? "file" : "memfd", fd, size, data);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	for (int f = 0; f < frames; f++) {
		if (!getenv("SHM_NOFILL")) for (int y = 0; y < H; y++) memset(data + (size_t)y * stride, (f * 7 + y) & 255, stride);
		wl_surface_attach(surf, buf, 0, 0);
		wl_surface_damage(surf, 0, 0, W, H);
		frame_done = 0;
		struct wl_callback *cb = wl_surface_frame(surf);
		wl_callback_add_listener(cb, &frame_listener, NULL);
		wl_surface_commit(surf);
		wl_display_flush(dpy);
		if (!getenv("SHM_NOWAIT"))
			while (!frame_done) if (wl_display_dispatch(dpy) < 0) { printf("shm-wl: dispatch failed at %d\n", f); return 1; }
		else
			wl_display_roundtrip(dpy);
		if (f < 5 || f % 100 == 99) printf("shm-wl: %d frames\n", f + 1);
	}
	printf("shm-wl: DONE %d frames\n", frames);
	return 0;
}
