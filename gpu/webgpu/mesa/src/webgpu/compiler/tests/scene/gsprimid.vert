#version 450
// an indexed, instanced triangle list into a GS (the vertex shader as compute reads the index buffer)
layout(location = 0) in vec2 p;       // R32G32_FLOAT, pulled
layout(location = 0) out vec3 col;
void main()
{
	gl_Position = vec4(p * 0.7 + vec2(gl_InstanceIndex - 3) * 0.25, 0.5, 1.0);
	col = vec3(float(gl_VertexIndex % 7) / 7.0, float(gl_InstanceIndex) / 5.0, 0.5);
}
