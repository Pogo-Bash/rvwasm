#version 450
// Sampling: implicit, explicit level, offset, gather, texel fetch, size; uniform buffer; discard.
layout(set = 0, binding = 0) uniform U { vec4 k[16]; };
layout(set = 0, binding = 1) uniform sampler2D t;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 vcol;
layout(location = 0) out vec4 o;
void main()
{
	vec4 a = texture(t, uv);
	vec4 b = textureLod(t, uv * 0.5, 0.0);
	vec4 c = textureOffset(t, uv, ivec2(2, -1));
	vec4 g = textureGather(t, uv, 1);
	ivec2 sz = textureSize(t, 0);
	vec4 f = texelFetch(t, ivec2(gl_FragCoord.xy) % sz, 0);
	if (a.b > 0.9 && uv.x > 1.5)
		discard;
	o = (a + b + c) * 0.25 + g * 0.0625 + f * 0.0625 + vcol * k[2].x;
}
