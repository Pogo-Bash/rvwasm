#version 450
#extension GL_GOOGLE_include_directive : require
#include "quad.glsl"
layout(location = 0) out vec2 p;
void main()
{
	vec2 corner;
	gl_Position = quad_pos(corner);
	p = corner * 8.0;
}
