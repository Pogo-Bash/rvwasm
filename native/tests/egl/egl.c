/*
 * EGL + GLES 2 through Mesa's virgl driver over vtest (VTEST_SOCKET_NAME, GALLIUM_DRIVER=virpipe), on the
 * surfaceless platform: what the driver says it is, then FRAMES frames of a clear and a triangle into a
 * 256x256 pbuffer, each read back with glReadPixels.
 *   egl-test [FRAMES]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
		printf("egl: shader: %s\n", log);
		exit(1);
	}
	return s;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 30;
	double t0 = now_ms();
	setvbuf(stdout, NULL, _IOLBF, 0);
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
		: EGL_NO_DISPLAY;
	EGLint major, minor;
	if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
		printf("egl: no display (0x%x)\n", eglGetError());
		return 1;
	}
	printf("egl: EGL %d.%d, %s\n", major, minor, eglQueryString(d, EGL_VENDOR));
	static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
		EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
	EGLConfig config;
	EGLint n;
	if (!eglChooseConfig(d, config_attribs, &config, 1, &n) || n < 1) {
		printf("egl: no config (0x%x)\n", eglGetError());
		return 1;
	}
	static const EGLint pbuffer_attribs[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
	EGLSurface surface = eglCreatePbufferSurface(d, config, pbuffer_attribs);
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, context_attribs);
	if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, surface, surface, c)) {
		printf("egl: no context (0x%x)\n", eglGetError());
		return 1;
	}
	printf("egl: GL_RENDERER %s\negl: GL_VERSION %s\negl: ready in %.0f ms\n", glGetString(GL_RENDERER),
		glGetString(GL_VERSION), now_ms() - t0);

	GLuint program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
		"attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
		"precision mediump float; uniform vec4 color; void main() { gl_FragColor = color; }"));
	glBindAttribLocation(program, 0, "p");
	glLinkProgram(program);
	glUseProgram(program);
	static const GLfloat triangle[] = { -0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, triangle);
	glEnableVertexAttribArray(0);
	GLint color = glGetUniformLocation(program, "color");

	static unsigned char pixels[256 * 256 * 4];
	double t1 = now_ms();
	for (int i = 0; i < frames; i++) {
		glClearColor(0.1f, 0.2f, 0.15f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUniform4f(color, (i % 10) / 10.0f, 0.8f, 0.3f, 1.0f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	}
	double t2 = now_ms();
	const unsigned char *mid = pixels + (128 * 256 + 128) * 4;
	printf("egl: %d frames in %.0f ms (%.1f fps); centre pixel %d,%d,%d,%d; error 0x%x\n", frames, t2 - t1,
		frames * 1000.0 / (t2 - t1), mid[0], mid[1], mid[2], mid[3], glGetError());
	eglTerminate(d);
	return 0;
}
