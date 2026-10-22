#version 460
// draw parameters with 52 bytes of push constants: an indexed draw with a base vertex and first instance
layout(push_constant) uniform PC { vec4 a[3]; uint b; } pc;
layout(location = 0) in vec2 pos;     // R32G32_FLOAT, native
layout(location = 0) flat out ivec4 ids;
layout(location = 1) out vec4 k;
void main()
{
	gl_Position = vec4(pos * 0.9 + vec2(gl_InstanceIndex - 2) * 0.05, 0.5, 1.0);
	ids = ivec4(gl_BaseVertex, gl_BaseInstance, gl_DrawID, gl_VertexIndex);
	k = pc.a[1] * float(pc.b & 7u);
}
