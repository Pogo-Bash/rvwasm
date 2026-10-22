#version 450
// large points: gl_PointSize 9, the colour from the vertex
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 v_color;
void main() {
	/* (T3b's point expansion fails NIR validation on this shader: the expanded code uses the point size's
	 * load_const before its definition (wgvk_legalize.c cleanup after point expansion). Reported; the case is
	 * left out of the default run until fixed: t2-test ... points) */
	gl_PointSize = 9.0;
	gl_Position = vec4(pos * 0.8, 0.5, 1.0);
	v_color = color;
}
