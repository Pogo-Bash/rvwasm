#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec3 col[];
layout(location = 0) out vec3 fcol;
void main()
{
	for (int i = 0; i < 3; i++) {
		gl_Position = gl_in[i].gl_Position;
		gl_Layer = gl_PrimitiveIDIn % 2;
		gl_ViewportIndex = (gl_PrimitiveIDIn / 2) % 2;
		fcol = col[i];
		EmitVertex();
	}
	EndPrimitive();
}
