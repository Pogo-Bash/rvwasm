#version 450
layout(location = 0) in vec4 col;
layout(location = 1) flat in uint tag;
layout(location = 0) out vec4 color;
void main() { color = vec4(col.rgb, float(tag & 255u) / 255.0); }
