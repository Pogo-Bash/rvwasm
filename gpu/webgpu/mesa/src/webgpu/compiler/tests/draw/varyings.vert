#version 450
#extension GL_GOOGLE_include_directive : require
// Interpolation: perspective, noperspective (only declared so on the fragment side: the vertex shader's
// default must be overridden by the link key), flat integers, centroid; instance and vertex index.
#include "quad.glsl"
layout(location = 0) out vec4 color;
layout(location = 1) out vec2 np;
layout(location = 2) flat out ivec2 ids;
layout(location = 3) flat out uint bits;
layout(location = 4) centroid out float cen;
void main()
{
	vec2 corner;
	gl_Position = quad_pos(corner);
	color = vec4(corner, 1.0 - corner.x, 1.0);
	np = corner;
	ids = ivec2(gl_VertexIndex, gl_InstanceIndex);
	bits = uint(gl_VertexIndex) * 37u + 5u;
	cen = corner.x * corner.y;
}
