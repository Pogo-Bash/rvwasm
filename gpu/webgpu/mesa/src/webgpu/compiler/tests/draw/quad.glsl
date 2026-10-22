// (included by the vertex shaders) a quad per instance from gl_VertexIndex: instance 0 the left half of
// the target, instance 1 the right half; w differs per corner so perspective interpolation shows
vec4 quad_pos(out vec2 corner)
{
	const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
	corner = c[gl_VertexIndex % 6];
	float x0 = gl_InstanceIndex == 0 ? -1.0 : 0.0;
	vec2 p = vec2(x0 + corner.x * 0.95, -0.9 + corner.y * 1.8);
	float w = 1.0 + corner.x * 0.7 + corner.y * 0.3;
	return vec4(p * w, 0.25 * w, w);
}
