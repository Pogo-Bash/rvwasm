#version 460
#extension GL_EXT_multiview : require
// multiview: gl_ViewIndex in the vertex stage (wgvk renders each view in a WebGPU pass of its own; the
// shader reads the view from driver uniform view_index). The scene renders view 2 into layer 2.
layout(location = 0) in vec2 pos;
layout(location = 0) out vec4 c;
void main()
{
	gl_Position = vec4(pos * 0.8 + vec2(float(gl_ViewIndex) * 0.1 - 0.2, 0.0), 0.5, 1.0);
	c = vec4(float(gl_ViewIndex) * 0.25, 0.5, 0.0, 1.0);
}
