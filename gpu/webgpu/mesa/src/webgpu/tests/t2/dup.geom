#version 450
// each triangle twice: as it is, and moved and darkened (a dynamic number of indices: libpoly's main part)
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 0) in vec4 v_color[];
layout(location = 0) out vec4 g_color;
void main() {
	for (int k = 0; k < 2; k++) {
		for (int i = 0; i < 3; i++) {
			gl_Position = gl_in[i].gl_Position + vec4(0.3 * k, 0.2 * k, 0.0, 0.0);
			g_color = v_color[i] * (k == 0 ? 1.0 : 0.5);
			EmitVertex();
		}
		EndPrimitive();
	}
}
