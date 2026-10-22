#version 450
#extension GL_GOOGLE_include_directive : require
#include "grid.glsl"
layout(location = 0) flat out int idx;
layout(location = 1) out vec2 uv;
void main()
{
	vec2 corner;
	gl_Position = cell_pos(gl_VertexIndex, gl_InstanceIndex, corner);
	idx = gl_InstanceIndex;
	uv = corner;
}
