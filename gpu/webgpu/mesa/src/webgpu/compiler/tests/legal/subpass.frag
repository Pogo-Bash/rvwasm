#version 450
// input attachments
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput color_in;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform usubpassInput id_in;
layout(location = 0) out vec4 color;
void main() {
   color = subpassLoad(color_in) * float(subpassLoad(id_in).x);
}
