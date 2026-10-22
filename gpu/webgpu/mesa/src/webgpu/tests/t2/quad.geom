#version 450
// points to quads (wide-line and sprite style, as Blender's overlays): a static strip per point
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 v_color[];
layout(location = 0) out vec4 g_color;
void main() {
	vec4 c = gl_in[0].gl_Position;
	for (int i = 0; i < 4; i++) {
		gl_Position = c + vec4((i & 1) == 1 ? 0.12 : -0.12, (i & 2) == 2 ? 0.12 : -0.12, 0.0, 0.0);
		g_color = v_color[0];
		EmitVertex();
	}
}
