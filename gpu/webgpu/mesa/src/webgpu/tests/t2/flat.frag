#version 450
// flat colour: which vertex is the provoking one shows
layout(location = 0) flat in vec4 v_color;
layout(location = 0) out vec4 o;
void main() { o = v_color; }
