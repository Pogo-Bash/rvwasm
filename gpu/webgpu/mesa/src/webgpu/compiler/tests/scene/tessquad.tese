#version 450
// bilinear position with a bulge, colour interpolated
layout(quads, equal_spacing, ccw) in;
layout(location = 0) in vec4 tcol[];
layout(location = 1) patch in float bulge;
layout(location = 0) out vec4 col;
void main()
{
	vec2 uv = gl_TessCoord.xy;
	vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, uv.x);
	vec4 b = mix(gl_in[2].gl_Position, gl_in[3].gl_Position, uv.x);
	vec4 p = mix(a, b, uv.y);
	p.xy += bulge * sin(uv * 3.14159);
	gl_Position = p;
	col = mix(mix(tcol[0], tcol[1], uv.x), mix(tcol[2], tcol[3], uv.x), uv.y);
}
