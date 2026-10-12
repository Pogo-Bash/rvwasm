/* GL functions vrend refers to that WebGL2 does not have. Each logs the first time and does nothing;
 * vrend reaches them only when a feature check let it, so a line from here means the caps are wrong. */
#include <epoxy/gl.h>
#include <stdio.h>

#define MISSING(name) do { static int said; if (!said++) fprintf(stderr, "vrend-wasm: %s is not in WebGL2\n", name); } while (0)

/* Not GL, but missing all the same: Emscripten without threads has no pthread_setname_np (vrend names its
 * sync thread, which it does not start here). */
#include <pthread.h>
int pthread_setname_np(pthread_t thread, const char *name)
{
	(void)thread;
	(void)name;
	return 0;
}

/* -- generated below by gen-missing.py: do not edit below this line -- */
void glActiveShaderProgram(GLuint pipeline, GLuint program)
{
	(void)pipeline; (void)program; MISSING("glActiveShaderProgram");
}
void glAlphaFunc(GLenum func, GLclampf ref)
{
	(void)func; (void)ref; MISSING("glAlphaFunc");
}
void glBeginConditionalRender(GLuint id, GLenum mode)
{
	(void)id; (void)mode; MISSING("glBeginConditionalRender");
}
void glBeginConditionalRenderNV(GLuint id, GLenum mode)
{
	(void)id; (void)mode; MISSING("glBeginConditionalRenderNV");
}
void glBeginQueryIndexed(GLenum target, GLuint index, GLuint id)
{
	(void)target; (void)index; (void)id; MISSING("glBeginQueryIndexed");
}
void glBindBufferARB(GLenum target, GLuint buffer)
{
	glBindBuffer(target, buffer);
}
void glBindFragDataLocationIndexed(GLuint program, GLuint colorNumber, GLuint index, const GLchar *name)
{
	(void)program; (void)colorNumber; (void)index; (void)name; MISSING("glBindFragDataLocationIndexed");
}
void glBindFragDataLocationIndexedEXT(GLuint program, GLuint colorNumber, GLuint index, const GLchar *name)
{
	(void)program; (void)colorNumber; (void)index; (void)name; MISSING("glBindFragDataLocationIndexedEXT");
}
void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format)
{
	(void)unit; (void)texture; (void)level; (void)layered; (void)layer; (void)access; (void)format; MISSING("glBindImageTexture");
}
void glBindProgramPipeline(GLuint pipeline)
{
	(void)pipeline; MISSING("glBindProgramPipeline");
}
void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride)
{
	(void)bindingindex; (void)buffer; (void)offset; (void)stride; MISSING("glBindVertexBuffer");
}
void glBindVertexBuffers(GLuint first, GLsizei count, const GLuint *buffers, const GLintptr *offsets, const GLsizei *strides)
{
	(void)first; (void)count; (void)buffers; (void)offsets; (void)strides; MISSING("glBindVertexBuffers");
}
void glBlendBarrierKHR(void)
{
	MISSING("glBlendBarrierKHR");
}
void glBlendEquationSeparateiARB(GLuint buf, GLenum modeRGB, GLenum modeAlpha)
{
	(void)buf; (void)modeRGB; (void)modeAlpha; MISSING("glBlendEquationSeparateiARB");
}
void glBlendFuncSeparateiARB(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)
{
	(void)buf; (void)srcRGB; (void)dstRGB; (void)srcAlpha; (void)dstAlpha; MISSING("glBlendFuncSeparateiARB");
}
void glBufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags)
{
	(void)target; (void)size; (void)data; (void)flags; MISSING("glBufferStorage");
}
void glClampColor(GLenum target, GLenum clamp)
{
	(void)target; (void)clamp; MISSING("glClampColor");
}
void glClearTexSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *data)
{
	(void)texture; (void)level; (void)xoffset; (void)yoffset; (void)zoffset; (void)width; (void)height; (void)depth; (void)format; (void)type; (void)data; MISSING("glClearTexSubImage");
}
void glClearTexSubImageEXT(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *data)
{
	(void)texture; (void)level; (void)xoffset; (void)yoffset; (void)zoffset; (void)width; (void)height; (void)depth; (void)format; (void)type; (void)data; MISSING("glClearTexSubImageEXT");
}
void glClipControl(GLenum origin, GLenum depth)
{
	(void)origin; (void)depth; MISSING("glClipControl");
}
void glClipPlane(GLenum plane, const GLdouble *equation)
{
	(void)plane; (void)equation; MISSING("glClipPlane");
}
void glColorMaskIndexedEXT(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	(void)index; (void)r; (void)g; (void)b; (void)a; MISSING("glColorMaskIndexedEXT");
}
void glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const GLvoid *data)
{
	(void)target; (void)level; (void)xoffset; (void)width; (void)format; (void)imageSize; (void)data; MISSING("glCompressedTexSubImage1D");
}
void glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth)
{
	(void)srcName; (void)srcTarget; (void)srcLevel; (void)srcX; (void)srcY; (void)srcZ; (void)dstName; (void)dstTarget; (void)dstLevel; (void)dstX; (void)dstY; (void)dstZ; (void)srcWidth; (void)srcHeight; (void)srcDepth; MISSING("glCopyImageSubData");
}
void glCreateMemoryObjectsEXT(GLsizei n, GLuint *memoryObjects)
{
	(void)n; (void)memoryObjects; MISSING("glCreateMemoryObjectsEXT");
}
void glDebugMessageCallback(GLDEBUGPROC callback, const void *userParam)
{
	(void)callback; (void)userParam; MISSING("glDebugMessageCallback");
}
void glDebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *buf)
{
	(void)source; (void)type; (void)id; (void)severity; (void)length; (void)buf; MISSING("glDebugMessageInsert");
}
void glDebugMessageInsertKHR(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *buf)
{
	(void)source; (void)type; (void)id; (void)severity; (void)length; (void)buf; MISSING("glDebugMessageInsertKHR");
}
void glDeleteMemoryObjectsEXT(GLsizei n, const GLuint *memoryObjects)
{
	(void)n; (void)memoryObjects; MISSING("glDeleteMemoryObjectsEXT");
}
void glDeleteProgramPipelines(GLsizei n, const GLuint *pipelines)
{
	(void)n; (void)pipelines; MISSING("glDeleteProgramPipelines");
}
void glDepthRangeIndexed(GLuint index, GLdouble n, GLdouble f)
{
	(void)index; (void)n; (void)f; MISSING("glDepthRangeIndexed");
}
void glDepthRangeIndexedfOES(GLuint index, GLfloat n, GLfloat f)
{
	(void)index; (void)n; (void)f; MISSING("glDepthRangeIndexedfOES");
}
void glDepthRangefOES(GLclampf n, GLclampf f)
{
	glDepthRangef(n, f);
}
void glDisableClientState(GLenum cap)
{
	(void)cap; MISSING("glDisableClientState");
}
void glDisableIndexedEXT(GLenum target, GLuint index)
{
	(void)target; (void)index; MISSING("glDisableIndexedEXT");
}
void glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z)
{
	(void)num_groups_x; (void)num_groups_y; (void)num_groups_z; MISSING("glDispatchCompute");
}
void glDispatchComputeIndirect(GLintptr indirect)
{
	(void)indirect; MISSING("glDispatchComputeIndirect");
}
void glDrawArraysIndirect(GLenum mode, const void *indirect)
{
	(void)mode; (void)indirect; MISSING("glDrawArraysIndirect");
}
void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
	(void)mode; (void)count; (void)type; (void)indices; (void)basevertex; MISSING("glDrawElementsBaseVertex");
}
void glDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect)
{
	(void)mode; (void)type; (void)indirect; MISSING("glDrawElementsIndirect");
}
void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLuint baseinstance)
{
	(void)mode; (void)count; (void)type; (void)indices; (void)instancecount; (void)baseinstance; MISSING("glDrawElementsInstancedBaseInstance");
}
void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex)
{
	(void)mode; (void)count; (void)type; (void)indices; (void)instancecount; (void)basevertex; MISSING("glDrawElementsInstancedBaseVertex");
}
void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex, GLuint baseinstance)
{
	(void)mode; (void)count; (void)type; (void)indices; (void)instancecount; (void)basevertex; (void)baseinstance; MISSING("glDrawElementsInstancedBaseVertexBaseInstance");
}
void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
	(void)width; (void)height; (void)format; (void)type; (void)pixels; MISSING("glDrawPixels");
}
void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
	(void)mode; (void)start; (void)end; (void)count; (void)type; (void)indices; (void)basevertex; MISSING("glDrawRangeElementsBaseVertex");
}
void glEGLImageTargetTexStorageEXT(GLenum target, GLeglImageOES image, const GLint* attrib_list)
{
	(void)target; (void)image; (void)attrib_list; MISSING("glEGLImageTargetTexStorageEXT");
}
void glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image)
{
	(void)target; (void)image; MISSING("glEGLImageTargetTexture2DOES");
}
void glEnableClientState(GLenum cap)
{
	(void)cap; MISSING("glEnableClientState");
}
void glEnableIndexedEXT(GLenum target, GLuint index)
{
	(void)target; (void)index; MISSING("glEnableIndexedEXT");
}
void glEndConditionalRender(void)
{
	MISSING("glEndConditionalRender");
}
void glEndConditionalRenderNV(void)
{
	MISSING("glEndConditionalRenderNV");
}
void glEndQueryIndexed(GLenum target, GLuint index)
{
	(void)target; (void)index; MISSING("glEndQueryIndexed");
}
void glFramebufferParameteri(GLenum target, GLenum pname, GLint param)
{
	(void)target; (void)pname; (void)param; MISSING("glFramebufferParameteri");
}
void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level)
{
	(void)target; (void)attachment; (void)texture; (void)level; MISSING("glFramebufferTexture");
}
void glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	(void)target; (void)attachment; (void)textarget; (void)texture; (void)level; MISSING("glFramebufferTexture1D");
}
void glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLsizei samples)
{
	(void)target; (void)attachment; (void)textarget; (void)texture; (void)level; (void)samples; MISSING("glFramebufferTexture2DMultisampleEXT");
}
void glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset)
{
	(void)target; (void)attachment; (void)textarget; (void)texture; (void)level; (void)zoffset; MISSING("glFramebufferTexture3D");
}
void glFramebufferTexture3DOES(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset)
{
	(void)target; (void)attachment; (void)textarget; (void)texture; (void)level; (void)zoffset; MISSING("glFramebufferTexture3DOES");
}
void glGenBuffersARB(GLsizei n, GLuint *buffers)
{
	glGenBuffers(n, buffers);
}
void glGenProgramPipelines(GLsizei n, GLuint *pipelines)
{
	(void)n; (void)pipelines; MISSING("glGenProgramPipelines");
}
void glGetCompressedTexImage(GLenum target, GLint lod, GLvoid *img)
{
	(void)target; (void)lod; (void)img; MISSING("glGetCompressedTexImage");
}
void glGetMultisamplefv(GLenum pname, GLuint index, GLfloat *val)
{
	(void)pname; (void)index; (void)val; MISSING("glGetMultisamplefv");
}
void glGetProgramPipelineiv(GLuint pipeline, GLenum pname, GLint *params)
{
	(void)pipeline; (void)pname; (void)params; MISSING("glGetProgramPipelineiv");
}
void glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64 *params)
{
	(void)id; (void)pname; (void)params; MISSING("glGetQueryObjecti64v");
}
void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params)
{
	(void)id; (void)pname; (void)params; MISSING("glGetQueryObjectiv");
}
void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params)
{
	(void)id; (void)pname; (void)params; MISSING("glGetQueryObjectui64v");
}
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
	(void)target; (void)level; (void)format; (void)type; (void)pixels; MISSING("glGetTexImage");
}
void glGetnCompressedTexImageARB(GLenum target, GLint lod, GLsizei bufSize, void *img)
{
	(void)target; (void)lod; (void)bufSize; (void)img; MISSING("glGetnCompressedTexImageARB");
}
void glGetnTexImageARB(GLenum target, GLint level, GLenum format, GLenum type, GLsizei bufSize, void *img)
{
	(void)target; (void)level; (void)format; (void)type; (void)bufSize; (void)img; MISSING("glGetnTexImageARB");
}
void glImportMemoryFdEXT(GLuint memory, GLuint64 size, GLenum handleType, GLint fd)
{
	(void)memory; (void)size; (void)handleType; (void)fd; MISSING("glImportMemoryFdEXT");
}
void glLineStipple(GLint factor, GLushort pattern)
{
	(void)factor; (void)pattern; MISSING("glLineStipple");
}
void glLogicOp(GLenum opcode)
{
	(void)opcode; MISSING("glLogicOp");
}
void glMemoryBarrier(GLbitfield barriers)
{
	(void)barriers; MISSING("glMemoryBarrier");
}
void glMemoryObjectParameterivEXT(GLuint memoryObject, GLenum pname, const GLint *params)
{
	(void)memoryObject; (void)pname; (void)params; MISSING("glMemoryObjectParameterivEXT");
}
void glMinSampleShading(GLfloat value)
{
	(void)value; MISSING("glMinSampleShading");
}
void glMultiDrawArraysIndirect(GLenum mode, const void *indirect, GLsizei drawcount, GLsizei stride)
{
	(void)mode; (void)indirect; (void)drawcount; (void)stride; MISSING("glMultiDrawArraysIndirect");
}
void glMultiDrawArraysIndirectCountARB(GLenum mode, const void *indirect, GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride)
{
	(void)mode; (void)indirect; (void)drawcount; (void)maxdrawcount; (void)stride; MISSING("glMultiDrawArraysIndirectCountARB");
}
void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride)
{
	(void)mode; (void)type; (void)indirect; (void)drawcount; (void)stride; MISSING("glMultiDrawElementsIndirect");
}
void glMultiDrawElementsIndirectCountARB(GLenum mode, GLenum type, const void *indirect, GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride)
{
	(void)mode; (void)type; (void)indirect; (void)drawcount; (void)maxdrawcount; (void)stride; MISSING("glMultiDrawElementsIndirectCountARB");
}
void glPatchParameterfv(GLenum pname, const GLfloat *values)
{
	(void)pname; (void)values; MISSING("glPatchParameterfv");
}
void glPatchParameteri(GLenum pname, GLint value)
{
	(void)pname; (void)value; MISSING("glPatchParameteri");
}
void glPixelTransferf(GLenum pname, GLfloat param)
{
	(void)pname; (void)param; MISSING("glPixelTransferf");
}
void glPixelZoom(GLfloat xfactor, GLfloat yfactor)
{
	(void)xfactor; (void)yfactor; MISSING("glPixelZoom");
}
void glPointParameteri(GLenum pname, GLint param)
{
	(void)pname; (void)param; MISSING("glPointParameteri");
}
void glPointSize(GLfloat size)
{
	(void)size; MISSING("glPointSize");
}
void glPolygonMode(GLenum face, GLenum mode)
{
	(void)face; (void)mode; MISSING("glPolygonMode");
}
void glPolygonStipple(const GLubyte *mask)
{
	(void)mask; MISSING("glPolygonStipple");
}
void glPrimitiveRestartIndex(GLuint index)
{
	(void)index; MISSING("glPrimitiveRestartIndex");
}
void glPrimitiveRestartIndexNV(GLuint index)
{
	(void)index; MISSING("glPrimitiveRestartIndexNV");
}
void glProvokingVertexEXT(GLenum mode)
{
	(void)mode; MISSING("glProvokingVertexEXT");
}
void glQueryCounter(GLuint id, GLenum target)
{
	(void)id; (void)target; MISSING("glQueryCounter");
}
void glReadnPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, void *data)
{
	(void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)bufSize; (void)data; MISSING("glReadnPixels");
}
void glReadnPixelsARB(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, void *data)
{
	(void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)bufSize; (void)data; MISSING("glReadnPixelsARB");
}
void glReadnPixelsKHR(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, void *data)
{
	(void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)bufSize; (void)data; MISSING("glReadnPixelsKHR");
}
void glRenderbufferStorageMultisampleEXT(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
{
	glRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}
void glSampleMaski(GLuint maskNumber, GLbitfield mask)
{
	(void)maskNumber; (void)mask; MISSING("glSampleMaski");
}
void glSamplerParameterIuiv(GLuint sampler, GLenum pname, const GLuint *param)
{
	(void)sampler; (void)pname; (void)param; MISSING("glSamplerParameterIuiv");
}
void glScissorIndexed(GLuint index, GLint left, GLint bottom, GLsizei width, GLsizei height)
{
	(void)index; (void)left; (void)bottom; (void)width; (void)height; MISSING("glScissorIndexed");
}
void glShadeModel(GLenum mode)
{
	(void)mode; MISSING("glShadeModel");
}
void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer)
{
	(void)target; (void)internalformat; (void)buffer; MISSING("glTexBuffer");
}
void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	(void)target; (void)internalformat; (void)buffer; (void)offset; (void)size; MISSING("glTexBufferRange");
}
void glTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
	(void)target; (void)level; (void)internalFormat; (void)width; (void)border; (void)format; (void)type; (void)pixels; MISSING("glTexImage1D");
}
void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations)
{
	(void)target; (void)samples; (void)internalformat; (void)width; (void)height; (void)fixedsamplelocations; MISSING("glTexImage2DMultisample");
}
void glTexImage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations)
{
	(void)target; (void)samples; (void)internalformat; (void)width; (void)height; (void)depth; (void)fixedsamplelocations; MISSING("glTexImage3DMultisample");
}
void glTexParameterIuiv(GLenum target, GLenum pname, const GLuint *params)
{
	(void)target; (void)pname; (void)params; MISSING("glTexParameterIuiv");
}
void glTexStorage1D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width)
{
	(void)target; (void)levels; (void)internalformat; (void)width; MISSING("glTexStorage1D");
}
void glTexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations)
{
	(void)target; (void)samples; (void)internalformat; (void)width; (void)height; (void)fixedsamplelocations; MISSING("glTexStorage2DMultisample");
}
void glTexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations)
{
	(void)target; (void)samples; (void)internalformat; (void)width; (void)height; (void)depth; (void)fixedsamplelocations; MISSING("glTexStorage3DMultisample");
}
void glTexStorageMem2DEXT(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset)
{
	(void)target; (void)levels; (void)internalFormat; (void)width; (void)height; (void)memory; (void)offset; MISSING("glTexStorageMem2DEXT");
}
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels)
{
	(void)target; (void)level; (void)xoffset; (void)width; (void)format; (void)type; (void)pixels; MISSING("glTexSubImage1D");
}
void glTextureBarrier(void)
{
	MISSING("glTextureBarrier");
}
void glTextureView(GLuint texture, GLenum target, GLuint origtexture, GLenum internalformat, GLuint minlevel, GLuint numlevels, GLuint minlayer, GLuint numlayers)
{
	(void)texture; (void)target; (void)origtexture; (void)internalformat; (void)minlevel; (void)numlevels; (void)minlayer; (void)numlayers; MISSING("glTextureView");
}
void glUseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program)
{
	(void)pipeline; (void)stages; (void)program; MISSING("glUseProgramStages");
}
void glValidateProgramPipeline(GLuint pipeline)
{
	(void)pipeline; MISSING("glValidateProgramPipeline");
}
void glVertexAttribBinding(GLuint attribindex, GLuint bindingindex)
{
	(void)attribindex; (void)bindingindex; MISSING("glVertexAttribBinding");
}
void glVertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset)
{
	(void)attribindex; (void)size; (void)type; (void)normalized; (void)relativeoffset; MISSING("glVertexAttribFormat");
}
void glVertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset)
{
	(void)attribindex; (void)size; (void)type; (void)relativeoffset; MISSING("glVertexAttribIFormat");
}
void glVertexBindingDivisor(GLuint bindingindex, GLuint divisor)
{
	(void)bindingindex; (void)divisor; MISSING("glVertexBindingDivisor");
}
void glViewportIndexedf(GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
{
	(void)index; (void)x; (void)y; (void)w; (void)h; MISSING("glViewportIndexedf");
}
void glWindowPos2i(GLint x, GLint y)
{
	(void)x; (void)y; MISSING("glWindowPos2i");
}
