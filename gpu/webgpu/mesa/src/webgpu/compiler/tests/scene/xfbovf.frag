#version 450
layout(location = 0) in vec4 col;
layout(location = 1) in vec4 extra;
layout(location = 0) out vec4 color;
void main() { color = vec4(col.rgb, extra.w); }
