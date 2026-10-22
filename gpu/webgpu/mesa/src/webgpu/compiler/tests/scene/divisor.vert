#version 450
#extension GL_GOOGLE_include_directive : require
// per-instance attributes with divisors 2 and 0, a 3-component unorm8 attribute (pulled), a native one
#include "grid.glsl"
layout(location = 0) in vec3 c0;      // binding 0, per vertex, R8G8B8_UNORM (no WebGPU format)
layout(location = 1) in ivec2 c1;     // binding 1, per instance, divisor 2, R16G16_SINT
layout(location = 2) in vec4 c2;      // binding 2, per vertex, R8G8B8A8_UNORM (native)
layout(location = 3) in uvec4 c3;     // binding 3, per instance, divisor 0, R8G8B8A8_UINT
layout(location = 0) out vec4 col;
void main()
{
	vec2 corner;
	gl_Position = cell_pos(gl_VertexIndex, gl_InstanceIndex, corner);
	col = vec4(c0.x, float(c1.x & 255) / 255.0, c2.z, float(c3.y) / 255.0) * 0.5 +
	      vec4(c2.x, float(c1.y & 255) / 255.0, c0.z, 0.5) * 0.5;
}
