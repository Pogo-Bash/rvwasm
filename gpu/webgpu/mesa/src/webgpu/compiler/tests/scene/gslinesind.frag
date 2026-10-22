#version 450
layout(location = 0) in vec4 fcol;
layout(location = 1) noperspective in vec2 edge;
layout(location = 0) out vec4 color;
void main() { color = vec4(fcol.rgb * (1.0 - abs(edge.x) * 0.5), 1.0); }
