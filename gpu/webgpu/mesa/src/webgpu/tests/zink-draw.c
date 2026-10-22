/*
 * GL drawing through zink on wgvk (T2): EGL surfaceless, a GLES 3 context,
 * an FBO; a clear, a vertex-coloured triangle, a textured quad, a uniform
 * buffer, depth, then glReadPixels and checks at pixels away from edges. Run
 * by gpu/webgpu/tests/zink.test when the build has zink (GPU_NATIVE=zink),
 * with LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=zink as there.
 *
 *   zink-draw [OUT.ppm]
 *
 * SPDX-License-Identifier: MIT
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S 64

static GLuint
program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	const char *src[2] = { vs, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);
		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		GLint ok;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[1024];
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			printf("FAIL zink-draw: shader: %s\n", log);
			exit(1);
		}
		glAttachShader(p, s);
	}
	glLinkProgram(p);
	GLint ok;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		printf("FAIL zink-draw: link\n");
		exit(1);
	}
	return p;
}

static float
ndc(float px)
{
	return px / (S / 2) - 1.0f;
}

int
main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
				     : EGL_NO_DISPLAY;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		printf("FAIL zink-draw: no surfaceless EGL display\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLConfig cfg;
	EGLint n = 0;
	const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
	if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || !n) {
		/* surfaceless contexts need no surface type */
		const EGLint cb[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
		eglChooseConfig(dpy, cb, &cfg, 1, &n);
	}
	const EGLint attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		printf("FAIL zink-draw: no GLES 3 context (0x%x)\n", eglGetError());
		return 1;
	}
	const char *renderer = (const char *)glGetString(GL_RENDERER);

	GLuint fbo, rb[2];
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(2, rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, S, S);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
	glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, S, S);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("FAIL zink-draw: framebuffer incomplete\n");
		return 1;
	}
	glViewport(0, 0, S, S);
	glClearColor(0.1f, 0.1f, 0.1f, 1);
	glClearDepthf(1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (!getenv("ZD_NODEPTH"))
		glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);

	GLuint col = program("#version 300 es\n"
			     "layout(location = 0) in vec3 pos; layout(location = 1) in vec4 c; out vec4 v;\n"
			     "void main() { gl_Position = vec4(pos, 1.0); v = c; }\n",
			     "#version 300 es\nprecision mediump float; in vec4 v; out vec4 o;\n"
			     "layout(std140) uniform U { vec4 tint; };\n"
			     "void main() { o = v * tint; }\n");
	GLuint tex = program("#version 300 es\n"
			     "layout(location = 0) in vec3 pos; layout(location = 1) in vec4 c; out vec2 uv;\n"
			     "void main() { gl_Position = vec4(pos, 1.0); uv = c.xy; }\n",
			     "#version 300 es\nprecision mediump float; in vec2 uv; uniform sampler2D t; out vec4 o;\n"
			     "void main() { o = texture(t, uv); }\n");

	/* quads (x0, y0, x1, y1 in pixels; GL's y is up, so y counts from the bottom here), z, rgba */
	struct { float x0, y0, x1, y1, z, c[4]; } q[3] = {
		{ 4, 4, 28, 28, 0.0f, { 1, 0, 0, 1 } },		/* A red, z 0 */
		{ 16, 16, 40, 40, -0.4f, { 0, 1, 0, 1 } },	/* B green, nearer */
		{ 36, 4, 60, 28, 0.0f, { 0, 0, 0, 0 } },	/* C textured: c = uv */
	};
	float v[3][4][7];
	for (int i = 0; i < 3; i++) {
		const float xs[4] = { q[i].x0, q[i].x1, q[i].x1, q[i].x0 }, ys[4] = { q[i].y0, q[i].y0, q[i].y1, q[i].y1 };
		const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 0, 0, 1, 1 };
		for (int k = 0; k < 4; k++) {
			float *p = v[i][k];
			p[0] = ndc(xs[k]);
			p[1] = ndc(ys[k]);
			p[2] = q[i].z;
			if (i == 2) {
				p[3] = us[k], p[4] = vs[k], p[5] = 0, p[6] = 1;
			} else {
				memcpy(p + 3, q[i].c, 16);
			}
		}
	}
	const GLushort idx[6] = { 0, 1, 2, 2, 3, 0 };
	GLuint vao;	/* first: the element array binding is the VAO's */
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	GLuint bufs[3];
	glGenBuffers(3, bufs);
	glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
	const float tint[4] = { 1, 1, 1, 1 };
	glBindBuffer(GL_UNIFORM_BUFFER, bufs[2]);
	glBufferData(GL_UNIFORM_BUFFER, 16, tint, GL_STATIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, bufs[2]);
	glUniformBlockBinding(col, glGetUniformBlockIndex(col, "U"), 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	/* the 2x2 texture: red green / blue white (row 0 is the bottom in GL: v = 0) */
	const unsigned char texels[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glUseProgram(col);
	for (int i = 0; i < 2; i++) {
		int which = i == 0 ? 1 : 0;	/* B first: A must lose the overlap by depth */
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void *)(uintptr_t)(which * 112));
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void *)(uintptr_t)(which * 112 + 12));
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
	}
	glUseProgram(tex);
	glUniform1i(glGetUniformLocation(tex, "t"), 0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void *)(uintptr_t)(2 * 112));
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void *)(uintptr_t)(2 * 112 + 12));
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

	static unsigned char px[S * S * 4];
	glReadPixels(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, px);
	GLenum err = glGetError();

	static const struct { int x, y; unsigned char c[4]; const char *what; } want[] = {
		{ 8, 8, { 255, 0, 0, 255 }, "A" },
		{ 22, 22, { 0, 255, 0, 255 }, "A/B overlap (depth)" },
		{ 36, 36, { 0, 255, 0, 255 }, "B" },
		{ 42, 10, { 255, 0, 0, 255 }, "C texel 0,0" },
		{ 54, 10, { 0, 255, 0, 255 }, "C texel 1,0" },
		{ 42, 22, { 0, 0, 255, 255 }, "C texel 0,1" },
		{ 54, 22, { 255, 255, 255, 255 }, "C texel 1,1" },
		{ 2, 62, { 26, 26, 26, 255 }, "background" },
	};
	int bad = 0;
	char msg[1024] = "";
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		const unsigned char *p = px + 4 * (want[i].y * S + want[i].x);
		bool ok = true;
		for (int c = 0; c < 4; c++)
			ok &= abs((int)p[c] - want[i].c[c]) <= 2;
		if (!ok) {
			bad++;
			size_t l = strlen(msg);
			snprintf(msg + l, sizeof(msg) - l, "%s at (%d,%d): %u %u %u %u; ", want[i].what, want[i].x, want[i].y,
				 p[0], p[1], p[2], p[3]);
		}
	}
	if (argc > 1) {
		FILE *f = fopen(argv[1], "wb");
		fprintf(f, "P6 %d %d 255\n", S, S);
		for (int y = S - 1; y >= 0; y--)
			for (int x = 0; x < S; x++)
				fwrite(px + 4 * (y * S + x), 1, 3, f);
		fclose(f);
	}
	if (bad || err)
		printf("FAIL zink-draw: %s: %d of 8 sample pixels wrong, GL error 0x%x: %s\n", renderer, bad, err, msg);
	else
		printf("ok zink-draw: %s: 8 sample pixels right (depth, texture, uniform buffer, indexed draws)\n", renderer);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	return bad || err ? 1 : 0;
}
