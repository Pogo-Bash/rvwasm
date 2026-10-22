#version 450
#extension GL_GOOGLE_include_directive : require
// vertex formats WebGPU's vertex state has no counterpart for, interleaved in one buffer
#include "grid.glsl"
layout(location = 0) in vec3 a;       // R16G16B16_SNORM @0
layout(location = 1) in vec4 b;       // R8G8B8A8_USCALED @6
layout(location = 2) in vec4 c;       // R10G10B10A2_SNORM @12
layout(location = 3) in uvec3 d;      // R8G8B8_UINT @16
layout(location = 4) in vec2 e;       // R16G16_SSCALED @20
layout(location = 0) out vec4 col;
void main()
{
	vec2 corner;
	gl_Position = cell_pos(gl_VertexIndex, gl_VertexIndex / 6, corner);
	col = vec4(a.x * 0.5 + 0.5, b.y / 255.0, c.z * 0.5 + 0.5, float(d.z) / 255.0) * 0.6 +
	      vec4(c.w * 0.5 + 0.5, abs(e.x) / 32768.0, a.z * 0.5 + 0.5, b.x / 255.0) * 0.4;
}
