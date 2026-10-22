#version 450
layout(location = 0) in vec4 col;
layout(location = 0) out vec4 color;
void main() { color = vec4(gl_PointCoord, col.z, 1.0); }
