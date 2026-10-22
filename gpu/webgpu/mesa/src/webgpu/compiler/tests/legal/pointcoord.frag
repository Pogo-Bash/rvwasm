#version 450
layout(location = 0) in vec4 c;
layout(location = 0) out vec4 color;
void main() {
   color = c * vec4(gl_PointCoord, 0.0, 1.0);
}
