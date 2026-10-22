#version 450
// straight-line code: a static topology (libpoly needs no compute pass to build the index buffer)
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 col[];
layout(location = 0) out vec4 fcol;
void main()
{
	vec4 c = gl_in[0].gl_Position;
	fcol = col[0]; gl_Position = c + vec4(-0.08, -0.08, 0, 0); EmitVertex();
	fcol = col[0] * 0.5; gl_Position = c + vec4(0.08, -0.08, 0, 0); EmitVertex();
	fcol = col[0]; gl_Position = c + vec4(-0.08, 0.08, 0, 0); EmitVertex();
	fcol = col[0].bgra; gl_Position = c + vec4(0.08, 0.08, 0, 0); EmitVertex();
	EndPrimitive();
}
