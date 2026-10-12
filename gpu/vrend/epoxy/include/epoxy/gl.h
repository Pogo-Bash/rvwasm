/* A stand-in for libepoxy's <epoxy/gl.h> for virglrenderer built with Emscripten: the GL ES 3.2 headers
 * (Emscripten's, with prototypes) for what WebGL2 has, gl_extra.h for the desktop GL names vrend refers to
 * behind its feature checks, and the three epoxy calls vrend makes (epoxy.c answers them from the WebGL2
 * context). Functions WebGL2 does not have are defined in missing.c: they log once and do nothing; vrend
 * only reaches them if a feature check said yes, so a log line from them is a bug in the caps. */
#ifndef RV_EPOXY_GL_H
#define RV_EPOXY_GL_H
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include "gl_extra.h"
#ifdef __cplusplus
extern "C" {
#endif
bool epoxy_has_gl_extension(const char *extension);
int epoxy_gl_version(void);
bool epoxy_is_desktop_gl(void);
#ifdef __cplusplus
}
#endif
#endif
