#version 450
// transform feedback from a vertex shader (no GS): two outputs into buffer 0, one into buffer 1
#extension GL_GOOGLE_include_directive : require
#include "grid.glsl"
layout(location = 0) in vec4 c;
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 32) out vec4 col;
layout(location = 1, xfb_buffer = 0, xfb_offset = 16) out vec4 extra;
layout(location = 2, xfb_buffer = 1, xfb_offset = 0, xfb_stride = 16) out vec4 pos2;
void main()
{
	vec2 corner;
	gl_Position = cell_pos(gl_VertexIndex, gl_InstanceIndex, corner);
	col = c;
	extra = vec4(float(gl_VertexIndex), float(gl_InstanceIndex), corner);
	pos2 = gl_Position * 2.0;
}
