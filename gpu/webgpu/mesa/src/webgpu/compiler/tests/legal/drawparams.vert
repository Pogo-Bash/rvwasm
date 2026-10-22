#version 460
// draw parameters with 52 bytes of push constants (zink's size): they go in the immediates after them
layout(push_constant) uniform PC { vec4 a[3]; uint b; } pc;
layout(location = 0) in vec4 pos;
layout(location = 0) flat out ivec4 ids;
void main() {
   gl_Position = pos + pc.a[0] * float(pc.b);
   ids = ivec4(gl_BaseVertex, gl_BaseInstance, gl_DrawID, gl_VertexIndex);
}
