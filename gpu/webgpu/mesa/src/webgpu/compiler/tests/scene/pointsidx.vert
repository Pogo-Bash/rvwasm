#version 450
// large points (odd sizes, centred on pixel centres) with a pulled colour, read by gl_PointCoord
layout(location = 0) in vec4 c;       // R8G8B8A8_UNORM, pulled (every attribute of an expanded point is)
layout(location = 0) out vec4 col;
void main()
{
	int i = gl_VertexIndex;
	vec2 px = vec2(6 + (i % 5) * 12, 6 + (i / 5) * 12) + 0.5;
	gl_Position = vec4(px / 32.0 - 1.0, 0.5, 1.0);
	gl_PointSize = float(3 + (i % 4) * 2);
	col = c;
}
