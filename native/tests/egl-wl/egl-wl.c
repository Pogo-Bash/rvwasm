/*
 * VK-18 repro: EGL on the Wayland platform (Mesa's swrast: softpipe through wl_shm buffers), GLES 2, FRAMES frames
 * of a moving triangle in an xdg toplevel of W x H, no Qt. The same buffer path Qt Quick's OpenGL backend takes.
 *   egl-wl [FRAMES [W [H]]]     env: EGLWL_SWAP_INTERVAL=0 (default 1), EGLWL_RESIZE=1 (resize every 50 frames)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"

#ifdef TRACE
void vk18_log(const char *fmt, ...);
void vk18_trace_announce(void);
#define TR(...) vk18_log(__VA_ARGS__)
#else
#define TR(...) ((void)0)
#endif
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static int configured;

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) { xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };
static void surf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	configured = 1;
}
static const struct xdg_surface_listener surf_listener = { surf_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {}
static void top_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	if (!strcmp(iface, "wl_compositor"))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
	else if (!strcmp(iface, "xdg_wm_base")) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static GLuint shader(GLenum type, const char *source)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		printf("egl-wl: shader: %s\n", log);
		exit(1);
	}
	return s;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 100;
	int W = argc > 2 ? atoi(argv[2]) : 420, H = argc > 3 ? atoi(argv[3]) : 320;
	setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef TRACE
	vk18_trace_announce();
#endif
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { printf("egl-wl: no wayland display\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !wm_base) { printf("egl-wl: missing globals\n"); return 1; }
	struct wl_surface *surf = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
	xdg_surface_add_listener(xs, &surf_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &top_listener, NULL);
	xdg_toplevel_set_title(top, "egl-wl");
	wl_surface_commit(surf);
	while (!configured)
		wl_display_dispatch(dpy);
	struct wl_egl_window *win = wl_egl_window_create(surf, W, H);

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = get_display(EGL_PLATFORM_WAYLAND_KHR, dpy, NULL);
	EGLint major, minor;
	if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
		printf("egl-wl: no display (0x%x)\n", eglGetError());
		return 1;
	}
	printf("egl-wl: EGL %d.%d, %s\n", major, minor, eglQueryString(d, EGL_VENDOR));
	static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
		EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig config;
	EGLint n;
	if (!eglChooseConfig(d, config_attribs, &config, 1, &n) || n < 1) {
		printf("egl-wl: no config (0x%x)\n", eglGetError());
		return 1;
	}
	EGLSurface surface = eglCreateWindowSurface(d, config, (EGLNativeWindowType)win, NULL);
	if (surface == EGL_NO_SURFACE) { printf("egl-wl: no surface (0x%x)\n", eglGetError()); return 1; }
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, context_attribs);
	if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, surface, surface, c)) {
		printf("egl-wl: no context (0x%x)\n", eglGetError());
		return 1;
	}
	eglSwapInterval(d, getenv("EGLWL_SWAP_INTERVAL") ? atoi(getenv("EGLWL_SWAP_INTERVAL")) : 1);
	printf("egl-wl: GL_RENDERER %s\negl-wl: GL_VERSION %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	GLuint program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
		"attribute vec2 p; uniform float a; void main() { gl_Position = vec4(p + vec2(a, 0.0), 0.0, 1.0); }"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
		"precision mediump float; uniform vec4 color; void main() { gl_FragColor = color; }"));
	glBindAttribLocation(program, 0, "p");
	glLinkProgram(program);
	glUseProgram(program);
	static const GLfloat triangle[] = { -0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, triangle);
	glEnableVertexAttribArray(0);
	GLint color = glGetUniformLocation(program, "color"), off = glGetUniformLocation(program, "a");
	const int step = getenv("EGLWL_STEP") ? atoi(getenv("EGLWL_STEP")) : 9;
	printf("egl-wl: setup done, step %d\n", step);
	if (step == 0)
		return 0;
	double t0 = now_ms();
	for (int f = 0; f < frames; f++) {
		if (getenv("EGLWL_RESIZE") && f % 50 == 49) {
			W = 300 + (f * 37) % 200;
			H = 200 + (f * 53) % 200;
			wl_egl_window_resize(win, W, H, 0, 0);
		}
		if (getenv("EGLWL_NODRAW")) {
			TR("== frame %d: swapping (nodraw)\n", f);
			if (!eglSwapBuffers(d, surface)) { printf("egl-wl: swap failed 0x%x at frame %d\n", eglGetError(), f); return 1; }
			TR("== frame %d: swapped\n", f);
			continue;
		}
		glViewport(0, 0, W, H);
		glClearColor(0.2f, 0.35f, 0.25f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUniform4f(color, 0.2f, 0.8f, 0.3f, 1.0f);
		glUniform1f(off, 0.1f * ((f % 16) - 8) / 8.0f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		if (step == 1) {
			glFinish();
			printf("egl-wl: frame %d drawn and finished (no swap)\n", f);
			continue;
		}
		TR("== frame %d: swapping\n", f);
		if (f < 3)
			printf("egl-wl: frame %d drawn, swapping\n", f);
		if (!eglSwapBuffers(d, surface)) { printf("egl-wl: swap failed 0x%x at frame %d\n", eglGetError(), f); return 1; }
		if (f % 100 == 99)
			printf("egl-wl: %d frames, %.1f fps\n", f + 1, (f + 1) * 1000.0 / (now_ms() - t0));
	}
	printf("egl-wl: DONE %d frames in %.0f ms\n", frames, now_ms() - t0);
	return 0;
}
