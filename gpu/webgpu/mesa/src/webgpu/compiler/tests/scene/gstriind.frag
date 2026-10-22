#version 450
layout(location = 0) in vec3 fcol;
layout(location = 1) flat in int prim;
layout(location = 0) out vec4 color;
void main() { color = vec4(fcol, float(prim * 37 % 256) / 255.0); }
