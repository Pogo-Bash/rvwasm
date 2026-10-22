#version 450
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o;
void main() { o = vec4(gl_PointCoord, 0.0, 1.0) * 0.5 + v_color * 0.5; }
