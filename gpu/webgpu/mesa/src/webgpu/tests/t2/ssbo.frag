#version 450
// writes the pixel count into a storage buffer (a fragment-stage store), colour passes through
layout(location = 0) in vec4 v_color;
layout(set = 0, binding = 0) buffer S { uint count[]; } s;
layout(location = 0) out vec4 o;
void main() { atomicAdd(s.count[0], 1u); o = v_color; }
