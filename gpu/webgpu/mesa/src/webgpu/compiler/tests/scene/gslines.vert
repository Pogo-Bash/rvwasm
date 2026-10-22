#version 450
// line endpoints from gl_VertexIndex, a colour per vertex (pulled: the vertex shader runs as compute)
layout(location = 0) in vec4 c;
layout(location = 0) out vec4 col;
layout(location = 1) out float w;
void main()
{
	int i = gl_VertexIndex;
	float a = float(i / 2) * 0.55 + float(i % 2) * 2.2;
	gl_Position = vec4(cos(a) * 0.8, sin(a) * 0.8, 0.5, 1.0);
	col = c;
	w = 0.03 + float(i % 3) * 0.02;
}
