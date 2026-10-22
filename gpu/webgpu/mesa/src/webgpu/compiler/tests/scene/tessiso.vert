#version 450
// two isoline patches of 4 control points
layout(location = 0) out vec4 col;
void main()
{
	int p = gl_VertexIndex / 4, i = gl_VertexIndex % 4;
	gl_Position = vec4(float(i) * 0.5 - 0.8, float(p) * 0.8 - 0.6 + float(i & 1) * 0.2, 0.5, 1.0);
	col = vec4(float(p), float(i) / 3.0, 1.0, 1.0);
}
