#version 450
// a uniform buffer (dynamic offset in the test) tints the colour
layout(location = 0) in vec4 v_color;
layout(set = 0, binding = 0) uniform U { vec4 tint; } u;
layout(location = 0) out vec4 o;
void main() { o = v_color * u.tint; }
