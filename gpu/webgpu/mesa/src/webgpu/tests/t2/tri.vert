#version 450
// position + colour per vertex, a per-instance offset (binding 1), a push-constant offset and scale
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 inst_off;
layout(push_constant) uniform PC { vec2 off; float scale; float z; } pc;
layout(location = 0) out vec4 v_color;
void main() {
	gl_Position = vec4(pos * pc.scale + pc.off + inst_off, pc.z, 1.0);
	v_color = color;
}
