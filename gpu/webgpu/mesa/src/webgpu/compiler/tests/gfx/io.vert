#version 450
// Vertex I/O: attributes of three base types, vertex/instance index, a UBO matrix, flat integer outputs,
// clip distances, point size (dropped), outputs written in pieces.
layout(location = 0) in vec3 pos;
layout(location = 1) in ivec2 ia;
layout(location = 2) in uvec4 ua;
layout(set = 0, binding = 0) uniform U { mat4 mvp; vec4 tint; };
layout(location = 0) out vec4 uv;
layout(location = 1) flat out ivec4 iv;
layout(location = 2) noperspective out vec2 np;
layout(location = 3) out float sv;
out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[2]; };
void main()
{
	gl_Position = mvp * vec4(pos, 1.0);
	gl_PointSize = 4.0;
	gl_ClipDistance[0] = pos.x;
	gl_ClipDistance[1] = -pos.y;
	uv.xy = pos.xy * tint.xy;
	uv.zw = vec2(gl_VertexIndex, gl_InstanceIndex);
	iv = ivec4(ia, ivec2(ua.xy));
	np = vec2(ua.zw);
	sv = 0.5;
}
