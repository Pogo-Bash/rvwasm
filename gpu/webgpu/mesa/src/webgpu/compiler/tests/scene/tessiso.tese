#version 450
// isolines: a cubic Bezier per line, the lines spread over the patch's height
layout(isolines, equal_spacing) in;
layout(location = 0) in vec4 tcol[];
layout(location = 0) out vec4 col;
void main()
{
	float u = gl_TessCoord.x, v = gl_TessCoord.y;
	float a = (1 - u) * (1 - u) * (1 - u), b = 3 * u * (1 - u) * (1 - u), c = 3 * u * u * (1 - u), d = u * u * u;
	vec4 p = a * gl_in[0].gl_Position + b * gl_in[1].gl_Position + c * gl_in[2].gl_Position + d * gl_in[3].gl_Position;
	p.y += v * 0.3;
	gl_Position = p;
	col = mix(tcol[0], tcol[3], u) * (0.5 + v);
}
