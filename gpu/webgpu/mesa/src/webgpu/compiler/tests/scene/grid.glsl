// (included) instance i of a draw: a quad in cell i of a 4x4 grid; corner from gl_VertexIndex % 6
vec4 cell_pos(int vertex, int instance, out vec2 corner)
{
	const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
	corner = c[vertex % 6];
	vec2 cell = vec2(instance % 4, (instance / 4) % 4);
	return vec4((cell + 0.1 + corner * 0.8) / 2.0 - 1.0, 0.5, 1.0);
}
