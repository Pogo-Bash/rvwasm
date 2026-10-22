#version 450
// integer textures sampled through samplers (clamped, nearest), with a level and an offset
layout(set = 0, binding = 0) uniform usampler2D ids;
layout(set = 0, binding = 1) uniform isampler2DArray layers;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main()
{
	uvec4 a = texture(ids, uv), b = textureLod(ids, uv, 1.0);
	ivec4 c = textureOffset(layers, vec3(uv, 1.6), ivec2(1, -1));
	color = vec4(uvec4(a.x, b.y >> 4, uint(c.z) >> 8, uint(c.x)) & 255u) / 255.0;
}
