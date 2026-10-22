#version 450
layout(location = 0) in vec2 p;
layout(location = 0) out vec3 col;
void main()
{
	gl_Position = vec4(p * 1.2, 0.5 + 0.45 * p.x * p.y + 0.1 * float(gl_InstanceIndex), 1.0);
	col = vec3(float(gl_VertexIndex % 5) / 5.0, float(gl_InstanceIndex), 0.5);
}
