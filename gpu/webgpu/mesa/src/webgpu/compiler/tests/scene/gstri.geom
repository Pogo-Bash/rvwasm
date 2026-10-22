#version 450
// EEVEE's shape: triangles in, a triangle strip out, gl_ViewportIndex written (WebGPU: viewport 0 only)
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec3 col[];
layout(location = 0) out vec3 fcol;
layout(location = 1) flat out int prim;
void main()
{
	for (int i = 0; i < 3; i++) {
		gl_Position = gl_in[i].gl_Position;
		gl_ViewportIndex = 0;
		fcol = col[i];
		prim = gl_PrimitiveIDIn;
		EmitVertex();
	}
	EndPrimitive();
}
