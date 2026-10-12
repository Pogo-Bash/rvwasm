/* The three epoxy calls vrend makes, answered from the current WebGL2 context through Emscripten's GL. */
#include <epoxy/gl.h>
#include <string.h>

/* GL ES 3.0: what WebGL2 is. vrend decides its features from this and the extensions below. */
int epoxy_gl_version(void)
{
	return 30;
}

bool epoxy_is_desktop_gl(void)
{
	return false;
}

/* Emscripten lists the WebGL extensions it enabled with a "GL_" in front (EXT_color_buffer_float is
 * GL_EXT_color_buffer_float), which is how vrend spells them. */
bool epoxy_has_gl_extension(const char *extension)
{
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++) {
		const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);
		if (e && !strcmp(e, extension))
			return true;
	}
	return false;
}
