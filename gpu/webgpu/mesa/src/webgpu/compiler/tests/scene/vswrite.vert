#version 450
// a vertex shader that writes a storage buffer (atomics and stores): WebGPU's vertex stage cannot, so it
// runs as compute and a passthrough vertex shader rasterizes its outputs
#extension GL_GOOGLE_include_directive : require
#include "grid.glsl"
layout(set = 0, binding = 0) buffer Log { uint count; uint ids[]; } lg;
layout(location = 0) in vec4 c;
layout(location = 0) out vec4 col;
layout(location = 1) flat out uint tag;
void main()
{
	vec2 corner;
	gl_Position = cell_pos(gl_VertexIndex, gl_InstanceIndex, corner);
	atomicAdd(lg.count, 1u);
	lg.ids[gl_InstanceIndex * 6 + gl_VertexIndex % 6] = uint(gl_VertexIndex);
	col = c * vec4(corner, 1.0, 1.0);
	tag = uint(gl_InstanceIndex * 40 + gl_VertexIndex);
}
