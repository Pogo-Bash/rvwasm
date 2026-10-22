#version 450
// Fragment builtins: frag coord, front facing, sample id / mask in and out, early return paths, loops with
// discard, integer and dual-purpose outputs.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 c0;
layout(set = 0, binding = 0) uniform U { vec4 k[4]; int n; };
void main()
{
	vec4 acc = vec4(0.0);
	for (int i = 0; i < n; i++) {
		acc += k[i & 3] * float(i);
		if (acc.x > 100.0)
			break;
	}
	if (!gl_FrontFacing)
		acc = -acc;
	acc.xy += gl_FragCoord.xy * 0.001 + float(gl_SampleID);
	gl_SampleMask[0] = gl_SampleMaskIn[0] & 5;
	if (acc.w < -1.0)
		discard;
	c0 = acc + vec4(uv, 0.0, 1.0);
}
