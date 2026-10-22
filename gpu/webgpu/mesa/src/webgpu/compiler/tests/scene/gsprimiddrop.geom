// glxgears through zink: the GS writes gl_PrimitiveID, the fragment shader does not read it
#version 450
// a GS writing gl_PrimitiveID (zink's GS for flat shading / polygon modes passes it on): the fragment
// shader must read the GS's value, not the rasterized draw's primitive index
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec3 col[];
layout(location = 0) out vec3 fcol;
void main()
{
	for (int i = 0; i < 3; i++) {
		gl_Position = gl_in[i].gl_Position;
		gl_PrimitiveID = gl_PrimitiveIDIn * 3 + 1;
		fcol = col[i];
		EmitVertex();
	}
	EndPrimitive();
}
