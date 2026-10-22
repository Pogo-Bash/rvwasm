#version 450
// Fragment control flow: loops with break/continue over interpolated values, phis, uniform buffer reads.
layout(set = 0, binding = 0) uniform U { vec4 k[16]; };
layout(location = 0) in vec2 p;
layout(location = 0) out vec4 o;
void main()
{
	vec2 z = vec2(0.0);
	int i;
	for (i = 0; i < 24; i++) {
		z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + (p - vec2(5.0, 4.0)) * 0.35;
		if (dot(z, z) > 4.0)
			break;
		if ((i & 3) == 1)
			continue;
		z *= 0.99 + k[i & 15].x * 0.01;
	}
	float s = float(i) / 24.0;
	o = vec4(s, fract(z.x), fract(z.y), 1.0 - s);
}
