#version 450
#extension GL_ARB_shader_draw_parameters : require
// 128 bytes of push constants (over the 64 bytes of immediates: the driver's binding 900) and gl_BaseInstance
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(push_constant) uniform PC { vec4 v[8]; } pc;
layout(location = 0) out vec4 v_color;
void main() {
	gl_Position = vec4(pos * pc.v[7].z + pc.v[7].xy + vec2(0.1 * float(gl_BaseInstanceARB), 0.0), 0.5, 1.0);
	v_color = color * pc.v[3];
}
