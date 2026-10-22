#version 450
// fewer output than input control points? no: 3 in, 3 out; each invocation reads its neighbour's output
// after a barrier, levels from the primitive and instance
layout(vertices = 3) out;
layout(location = 0) in vec4 col[];
layout(location = 0) out vec4 tcol[];
layout(location = 1) out float w[];
void main()
{
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	w[gl_InvocationID] = float(gl_InvocationID + 1);
	barrier();
	tcol[gl_InvocationID] = col[gl_InvocationID] * (0.5 + 0.1 * w[(gl_InvocationID + 1) % 3]);
	if (gl_InvocationID == 0) {
		float l = 1.5 + float(gl_PrimitiveID) * 1.3;
		gl_TessLevelOuter[0] = l;
		gl_TessLevelOuter[1] = 2.7;
		gl_TessLevelOuter[2] = l + 0.6;
		gl_TessLevelInner[0] = 3.4 + float(gl_PrimitiveID);
	}
}
