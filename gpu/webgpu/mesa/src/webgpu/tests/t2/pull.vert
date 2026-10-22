#version 450
// vertex pulling: a 3-byte colour, a packed 2x16 snorm position, an instance offset with divisor 2
layout(location = 0) in vec2 pos;      // R16G16_SNORM at offset 0 (stride 7: not a multiple of 4)
layout(location = 1) in vec3 color;    // R8G8B8_UNORM at offset 4
layout(location = 2) in vec2 inst_off; // binding 1, per instance, divisor 2
layout(location = 0) out vec4 v_color;
void main() {
	gl_Position = vec4(pos * 0.4 + inst_off, 0.5, 1.0);
	v_color = vec4(color, 1.0);
}
