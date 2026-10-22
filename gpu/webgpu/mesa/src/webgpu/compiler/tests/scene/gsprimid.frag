#version 450
layout(location = 0) in vec3 fcol;
layout(location = 0) out vec4 color;
void main() { color = vec4(fcol.xy, float(gl_PrimitiveID % 17) / 16.0, 1.0); }
