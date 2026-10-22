#version 450
#extension GL_EXT_nonuniform_qualifier : require
// dynamically indexed arrays of combined samplers, textures + samplers, UBOs and SSBOs
layout(set = 0, binding = 0) uniform sampler2D tex[5];
layout(set = 0, binding = 1) uniform texture2D t2[3];
layout(set = 0, binding = 2) uniform sampler smp;
layout(set = 1, binding = 0) uniform U { vec4 v; } ubos[4];
layout(set = 1, binding = 1) buffer S { vec4 w[]; } ssbos[3];
layout(location = 0) flat in int idx;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
   color = texture(tex[idx], uv) + texture(sampler2D(t2[idx % 3], smp), uv);
   color += ubos[nonuniformEXT(idx & 3)].v + ssbos[idx % 3].w[idx];
}
