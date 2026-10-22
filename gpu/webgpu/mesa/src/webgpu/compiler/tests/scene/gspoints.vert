#version 450
layout(location = 0) out vec4 col;
void main()
{
	int i = gl_VertexIndex;
	gl_Position = vec4(float(i % 6) / 3.5 - 0.75, float(i / 6) / 3.0 - 0.6, 0.5, 1.0);
	col = vec4(float(i) / 24.0, 1.0 - float(i) / 24.0, 0.3, 1.0);
}
