/*
 * drawbench: guest CPU per frame of a GL ES 3.0 program, to compare guest driver stacks (virgl vs zink+venus):
 * EGL surfaceless, a 512x512 FBO, each frame N draws with two uniform changes and a texture bind each, glFlush,
 * and every 8th frame a 1-pixel glReadPixels (a sync point, like a present). No GL headers: everything comes
 * from eglGetProcAddress, so it builds with a bare riscv64 cross compiler.
 *   drawbench DRAWS SECONDS
 * Prints frames, wall time and process CPU time (all threads) per frame.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef void *(*gpa_t)(const char *);
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum; typedef int GLsizei; typedef float GLfloat;
static double now(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }

#define L(type, name, args) type (*name) args = (type (*) args)gpa(#name); if (!name) { fprintf(stderr, "no %s\n", #name); return 1; }

static const char *vs = "#version 300 es\nuniform vec4 off;\nout vec2 uv;\nvoid main() { vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)); uv = p; gl_Position = vec4(p * 0.1 + off.xy, 0.0, 1.0); }\n";
static const char *fs = "#version 300 es\nprecision mediump float;\nuniform vec4 col;\nuniform sampler2D tex;\nin vec2 uv;\nout vec4 o;\nvoid main() { o = col * texture(tex, uv); }\n";

int main(int argc, char **argv)
{
	int draws = argc > 1 ? atoi(argv[1]) : 100;
	double secs = argc > 2 ? atof(argv[2]) : 20;
	void *egl = dlopen("libEGL.so.1", RTLD_NOW);
	if (!egl) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	gpa_t gpa = (gpa_t)dlsym(egl, "eglGetProcAddress");
	L(void *, eglGetPlatformDisplay, (GLenum, void *, const long *));
	L(int, eglInitialize, (void *, int *, int *));
	L(int, eglBindAPI, (GLenum));
	L(void *, eglCreateContext, (void *, void *, void *, const int *));
	L(int, eglMakeCurrent, (void *, void *, void *, void *));
	void *dpy = eglGetPlatformDisplay(0x31DD, NULL, NULL);
	int maj, min;
	if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
	eglBindAPI(0x30A0);
	int ca[] = { 0x3098, 3, 0x3038 };
	void *ctx = eglCreateContext(dpy, NULL, NULL, ca);
	if (!ctx || !eglMakeCurrent(dpy, NULL, NULL, ctx)) { fprintf(stderr, "no ES 3 context\n"); return 1; }
	L(const char *, glGetString, (GLenum));
	L(GLuint, glCreateShader, (GLenum)); L(void, glShaderSource, (GLuint, GLsizei, const char **, const GLint *));
	L(void, glCompileShader, (GLuint)); L(GLuint, glCreateProgram, (void)); L(void, glAttachShader, (GLuint, GLuint));
	L(void, glLinkProgram, (GLuint)); L(void, glUseProgram, (GLuint)); L(GLint, glGetUniformLocation, (GLuint, const char *));
	L(void, glGenTextures, (GLsizei, GLuint *)); L(void, glBindTexture, (GLenum, GLuint));
	L(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
	L(void, glTexParameteri, (GLenum, GLenum, GLint)); L(void, glGenFramebuffers, (GLsizei, GLuint *));
	L(void, glBindFramebuffer, (GLenum, GLuint)); L(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
	L(void, glViewport, (GLint, GLint, GLsizei, GLsizei)); L(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
	L(void, glClear, (GLenum)); L(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat));
	L(void, glUniform1i, (GLint, GLint)); L(void, glDrawArrays, (GLenum, GLint, GLsizei)); L(void, glFlush, (void));
	L(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)); L(GLenum, glGetError, (void));
	L(void, glGenVertexArrays, (GLsizei, GLuint *)); L(void, glBindVertexArray, (GLuint));
	printf("renderer: %s | %s\n", glGetString(0x1F01), glGetString(0x1F02));
	GLuint s1 = glCreateShader(0x8B31), s2 = glCreateShader(0x8B30), p = glCreateProgram();
	glShaderSource(s1, 1, &vs, NULL); glCompileShader(s1); glShaderSource(s2, 1, &fs, NULL); glCompileShader(s2);
	glAttachShader(p, s1); glAttachShader(p, s2); glLinkProgram(p); glUseProgram(p);
	GLint uoff = glGetUniformLocation(p, "off"), ucol = glGetUniformLocation(p, "col"), utex = glGetUniformLocation(p, "tex");
	glUniform1i(utex, 0);
	GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
	GLuint tex[5]; glGenTextures(5, tex);
	static unsigned char px[64 * 64 * 4];
	for (int i = 0; i < (int)sizeof px; i++) px[i] = i * 7;
	for (int i = 0; i < 4; i++) {
		glBindTexture(0x0DE1, tex[i]); glTexImage2D(0x0DE1, 0, 0x8058, 64, 64, 0, 0x1908, 0x1401, px);
		glTexParameteri(0x0DE1, 0x2801, 0x2601); glTexParameteri(0x0DE1, 0x2800, 0x2601);
	}
	glBindTexture(0x0DE1, tex[4]); glTexImage2D(0x0DE1, 0, 0x8058, 512, 512, 0, 0x1908, 0x1401, NULL);
	GLuint fb; glGenFramebuffers(1, &fb); glBindFramebuffer(0x8D40, fb);
	glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex[4], 0);
	glViewport(0, 0, 512, 512);
	unsigned char out[4];
	double w0 = 0, c0 = 0; int frames = 0, warm = 10;
	for (;;) {
		if (frames == warm) { w0 = now(CLOCK_MONOTONIC); c0 = now(CLOCK_PROCESS_CPUTIME_ID); }
		glClearColor(0.1, 0.2, 0.3, 1); glClear(0x4000);
		for (int i = 0; i < draws; i++) {
			glBindTexture(0x0DE1, tex[i & 3]);
			glUniform4f(uoff, (i % 17) * 0.1 - 0.9, (i % 13) * 0.1 - 0.9, 0, 0);
			glUniform4f(ucol, (i & 7) / 7.0, 0.5, 1 - (i & 3) / 3.0, 1);
			glDrawArrays(0x0004, 0, 3);
		}
		glFlush();
		if ((frames & 7) == 7) glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, out);
		frames++;
		if (frames > warm && now(CLOCK_MONOTONIC) - w0 > secs) break;
	}
	glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, out);
	double w = now(CLOCK_MONOTONIC) - w0, c = now(CLOCK_PROCESS_CPUTIME_ID) - c0;
	int n = frames - warm;
	printf("draws/frame %d frames %d wall %.2f s cpu %.2f s => %.2f ms wall/frame, %.2f ms cpu/frame, %.1f us cpu/draw, err 0x%x px %d,%d,%d\n",
		draws, n, w, c, w * 1000 / n, c * 1000 / n, c * 1e6 / n / draws, glGetError(), out[0], out[1], out[2]);
	return 0;
}
