#version 450
// passes the control points on and picks tessellation levels per patch
layout(vertices = 4) out;
layout(location = 0) in vec4 col[];
layout(location = 0) out vec4 tcol[];
layout(location = 1) patch out float bulge;
void main()
{
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	tcol[gl_InvocationID] = col[gl_InvocationID];
	if (gl_InvocationID == 0) {
		float l = 2.0 + float(gl_PrimitiveID % 3);
		gl_TessLevelOuter[0] = l;
		gl_TessLevelOuter[1] = l + 1.0;
		gl_TessLevelOuter[2] = 3.0;
		gl_TessLevelOuter[3] = l + 2.0;
		gl_TessLevelInner[0] = 3.0;
		gl_TessLevelInner[1] = l;
		bulge = 0.08 * float(gl_PrimitiveID + 1);
	}
}
