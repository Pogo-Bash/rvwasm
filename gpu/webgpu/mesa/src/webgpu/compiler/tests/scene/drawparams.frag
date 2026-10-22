#version 450
layout(location = 0) flat in ivec4 ids;
layout(location = 1) in vec4 k;
layout(location = 0) out vec4 color;
void main() { color = vec4(ids * ivec4(20, 30, 50, 7) & 255) / 255.0 + k * 0.0001; }
