#version 450
layout(location = 0) in vec3 fcol;
layout(location = 0) out vec4 color;
void main() { color = vec4(fcol, 1.0) + vec4(0.0, 0.0, float(gl_Layer) * 0.3, 0.0); }
