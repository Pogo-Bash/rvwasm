#version 450
#extension GL_GOOGLE_include_directive : require
#include "grid.glsl"
layout(location = 0) out vec2 uv;
void main()
{
	vec2 corner;
	gl_Position = cell_pos(gl_VertexIndex, gl_InstanceIndex, corner);
	uv = corner * 1.4 - 0.2 + vec2(gl_InstanceIndex) * 0.03;
}
