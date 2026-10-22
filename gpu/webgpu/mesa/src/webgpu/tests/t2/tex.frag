#version 450
// a combined image sampler, coordinates from the colour's rg
layout(location = 0) in vec4 v_color;
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texture(t, v_color.rg); }
