#version 450
// point mode, fractional even spacing: every tessellated vertex once, as a point
layout(triangles, fractional_even_spacing, point_mode) in;
layout(location = 0) in vec4 tcol[];
layout(location = 1) in float w[];
layout(location = 0) out vec4 col;
void main()
{
	vec3 t = gl_TessCoord;
	gl_Position = t.x * gl_in[0].gl_Position + t.y * gl_in[1].gl_Position + t.z * gl_in[2].gl_Position;
	gl_Position.y += 0.03 * sin(t.x * 9.0) * (w[0] + w[2]);
	col = t.x * tcol[0] + t.y * tcol[1] + t.z * tcol[2];
}
