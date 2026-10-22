#version 450
// a quad patch per instance: 4 control points around the cell's centre
layout(location = 0) out vec4 col;
void main()
{
	int i = gl_VertexIndex % 4, inst = gl_InstanceIndex;
	vec2 cell = vec2(inst % 2, inst / 2);
	vec2 c = vec2(i & 1, i >> 1);
	gl_Position = vec4((cell + 0.1 + c * 0.8) - 1.0, 0.5, 1.0);
	col = vec4(c, float(gl_VertexIndex) / 8.0, 1.0);
}
