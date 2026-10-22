#version 450
#extension GL_ARB_shader_viewport_layer_array : require
layout(location = 0) in vec2 p;
layout(location = 0) out vec3 col;
void main()
{
	int t = gl_InstanceIndex;
	gl_Position = vec4(p, 0.5, 1.0);
	gl_ViewportIndex = t % 3;
	gl_Layer = (t / 3) % 2;
	col = vec3(float(t) / 6.0, float(gl_VertexIndex % 3) / 3.0, 0.8);
}
