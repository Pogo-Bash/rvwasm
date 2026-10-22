#version 450
// a feedback loop: draws on the right half while sampling the left half of the image it renders to
layout(location = 0) in vec4 v_color;
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texture(t, vec2((gl_FragCoord.x - 32.0) / 64.0, gl_FragCoord.y / 48.0)) * vec4(1.0, 0.5, 1.0, 1.0); }
