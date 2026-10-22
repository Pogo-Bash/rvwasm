#version 450
#extension GL_GOOGLE_include_directive : require
// Texture coordinates, a texture read in the vertex stage (explicit level), uniform buffer values.
#include "quad.glsl"
layout(set = 0, binding = 0) uniform U { vec4 k[16]; };
layout(set = 0, binding = 1) uniform sampler2D t;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 vcol;
void main()
{
	vec2 corner;
	gl_Position = quad_pos(corner);
	uv = corner * 2.3 - vec2(0.4) + k[1].xy;
	vcol = textureLod(t, corner, 0.0) * k[3].w;
}
