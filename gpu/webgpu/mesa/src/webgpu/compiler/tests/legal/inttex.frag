#version 450
// integer textures sampled through a sampler (Blender's object id buffers)
layout(set = 0, binding = 0) uniform usampler2D ids;
layout(set = 0, binding = 1) uniform isampler2DArray layers;
layout(location = 0) in vec2 uv;
layout(location = 0) out uvec4 color;
void main() {
   color = texture(ids, uv) + textureLod(ids, uv, 1.0) + uvec4(textureOffset(layers, vec3(uv, 2.0), ivec2(1, -1)));
}
