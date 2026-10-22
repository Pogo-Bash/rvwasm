#version 450
layout(location = 0) in vec4 color;
layout(location = 1) noperspective in vec2 np;
layout(location = 2) flat in ivec2 ids;
layout(location = 3) flat in uint bits;
layout(location = 4) centroid in float cen;
layout(location = 0) out vec4 o;
void main()
{
	float f = fract(gl_FragCoord.x * 0.07 + gl_FragCoord.y * 0.013);
	o = vec4(color.x * 0.5 + np.x * 0.5, color.y * np.y, float(ids.y) * 0.5 + float(bits & 7u) / 16.0, cen * 0.5 + f * 0.5);
	if (!gl_FrontFacing)
		o.b = 1.0;
}
