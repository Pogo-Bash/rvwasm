#version 460
#extension GL_EXT_multiview : require
layout(location = 0) in vec4 c;
layout(location = 0) out vec4 color;
void main() { color = c + vec4(0.0, 0.0, float(gl_ViewIndex + 1) * 0.2, 0.0); }
