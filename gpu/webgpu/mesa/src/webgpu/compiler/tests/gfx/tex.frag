#version 450
// Every sampling form WGSL has: implicit/bias/lod/grad, offsets, array/cube/3D/cube-array, shadow (compare,
// compare at level 0, gather compare), gather with component and offset, texel fetch (float, int, uint, MS),
// split texture + sampler, size / levels / samples queries, derivatives, discard, frag depth, interpolation.
layout(set = 0, binding = 0) uniform sampler2D s2d;
layout(set = 0, binding = 1) uniform sampler2DArray s2da;
layout(set = 0, binding = 2) uniform samplerCube scube;
layout(set = 0, binding = 3) uniform sampler3D s3d;
layout(set = 0, binding = 4) uniform sampler2DShadow s2ds;
layout(set = 0, binding = 5) uniform sampler2DArrayShadow s2das;
layout(set = 0, binding = 6) uniform samplerCubeShadow scubes;
layout(set = 0, binding = 7) uniform isampler2D is2d;
layout(set = 0, binding = 8) uniform usampler2D us2d;
layout(set = 0, binding = 9) uniform sampler2DMS s2dms;
layout(set = 0, binding = 10) uniform texture2D t2d;
layout(set = 0, binding = 11) uniform sampler smp;
layout(set = 0, binding = 12) uniform samplerCubeArray scubea;
layout(location = 0) in vec4 uv;
layout(location = 1) flat in ivec4 iv;
layout(location = 2) noperspective centroid in vec2 np;
layout(location = 3) sample in float sv;
layout(location = 0) out vec4 c0;
layout(location = 1) out ivec4 c1;
void main()
{
	vec4 r = texture(s2d, uv.xy) + texture(s2d, uv.xy, 1.5) + textureLod(s2d, uv.xy, 2.0) +
	         textureGrad(s2d, uv.xy, vec2(0.1), vec2(0.2));
	r += textureOffset(s2d, uv.xy, ivec2(1, -2)) + textureLodOffset(s2d, uv.xy, 1.0, ivec2(3, 3));
	r += texture(s2da, uv.xyz) + texture(scube, uv.xyz) + texture(s3d, uv.xyz) + texture(scubea, uv);
	r.x += texture(s2ds, uv.xyz) + texture(s2das, uv) + texture(scubes, uv) + textureLod(s2ds, uv.xyz, 0.0);
	r += textureGather(s2d, uv.xy, 2) + textureGatherOffset(s2d, uv.xy, ivec2(1, 1)) + textureGather(s2ds, uv.xy, 0.5);
	r += texelFetch(s2d, iv.xy, 0) + vec4(texelFetch(is2d, iv.xy, 1)) + vec4(texelFetch(us2d, iv.xy, 0)) +
	     texelFetch(s2dms, iv.xy, 3);
	r += texture(sampler2D(t2d, smp), np);
	r.xy += vec2(textureSize(s2d, 1)) + vec2(textureSize(s2da, 0).z) + float(textureQueryLevels(s2d)) +
	        float(textureSamples(s2dms));
	r += dFdx(uv) + dFdyFine(uv) + fwidth(uv);
	if (uv.w < 0.0)
		discard;
	c0 = r * sv;
	c1 = iv;
	gl_FragDepth = uv.z;
}
