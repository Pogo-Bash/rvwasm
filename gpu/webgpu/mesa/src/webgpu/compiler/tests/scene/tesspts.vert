#version 450
// three triangle patches per instance (9 control points), two instances
layout(location = 0) out vec4 col;
void main()
{
	int p = gl_VertexIndex / 3, i = gl_VertexIndex % 3;
	vec2 o = vec2(float(p) * 0.6 - 0.9, float(gl_InstanceIndex) * 0.9 - 0.8);
	vec2 c = i == 0 ? vec2(0.0) : i == 1 ? vec2(0.55, 0.1) : vec2(0.2, 0.7);
	gl_Position = vec4(o + c, 0.5, 1.0);
	col = vec4(float(i == 0), float(i == 1), float(i == 2), 1.0);
}
