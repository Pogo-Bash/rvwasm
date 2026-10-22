#version 450
layout(vertices = 4) out;
layout(location = 0) in vec4 col[];
layout(location = 0) out vec4 tcol[];
void main()
{
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	tcol[gl_InvocationID] = col[gl_InvocationID];
	gl_TessLevelOuter[0] = 3.0 + float(gl_PrimitiveID);
	gl_TessLevelOuter[1] = 9.0;
}
