#version 450
layout(early_fragment_tests) in;
layout(set = 0, binding = 0, r32ui) uniform uimage2DArray counts;
layout(location = 0) in vec3 col;
layout(location = 0) out vec4 color;
void main()
{
	imageAtomicAdd(counts, ivec3(ivec2(gl_FragCoord.xy), 0), 1u);
	color = vec4(col, 1.0);
}
