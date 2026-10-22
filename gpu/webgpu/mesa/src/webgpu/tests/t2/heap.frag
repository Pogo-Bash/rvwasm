#version 450
// a fragment stage with 20 storage buffers (the buffer heap in a draw): the colour from their words
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) readonly buffer B0 { uint v[]; } b0;
layout(set = 0, binding = 1) readonly buffer B1 { uint v[]; } b1;
layout(set = 0, binding = 2) readonly buffer B2 { uint v[]; } b2;
layout(set = 0, binding = 3) readonly buffer B3 { uint v[]; } b3;
layout(set = 0, binding = 4) readonly buffer B4 { uint v[]; } b4;
layout(set = 0, binding = 5) readonly buffer B5 { uint v[]; } b5;
layout(set = 0, binding = 6) readonly buffer B6 { uint v[]; } b6;
layout(set = 0, binding = 7) readonly buffer B7 { uint v[]; } b7;
layout(set = 0, binding = 8) readonly buffer B8 { uint v[]; } b8;
layout(set = 0, binding = 9) readonly buffer B9 { uint v[]; } b9;
layout(set = 0, binding = 10) readonly buffer B10 { uint v[]; } b10;
layout(set = 0, binding = 11) readonly buffer B11 { uint v[]; } b11;
layout(set = 0, binding = 12) readonly buffer B12 { uint v[]; } b12;
layout(set = 0, binding = 13) readonly buffer B13 { uint v[]; } b13;
layout(set = 0, binding = 14) readonly buffer B14 { uint v[]; } b14;
layout(set = 0, binding = 15) readonly buffer B15 { uint v[]; } b15;
layout(set = 0, binding = 16) readonly buffer B16 { uint v[]; } b16;
layout(set = 0, binding = 17) readonly buffer B17 { uint v[]; } b17;
layout(set = 0, binding = 18) readonly buffer B18 { uint v[]; } b18;
layout(set = 0, binding = 19) readonly buffer B19 { uint v[]; } b19;
void main() {
	uint s = 0u;
	s += b0.v[0] * 1u;
	s += b1.v[1] * 2u;
	s += b2.v[2] * 3u;
	s += b3.v[3] * 4u;
	s += b4.v[4] * 5u;
	s += b5.v[5] * 6u;
	s += b6.v[6] * 7u;
	s += b7.v[7] * 8u;
	s += b8.v[8] * 9u;
	s += b9.v[9] * 10u;
	s += b10.v[10] * 11u;
	s += b11.v[11] * 12u;
	s += b12.v[12] * 13u;
	s += b13.v[13] * 14u;
	s += b14.v[14] * 15u;
	s += b15.v[15] * 16u;
	s += b16.v[16] * 17u;
	s += b17.v[17] * 18u;
	s += b18.v[18] * 19u;
	s += b19.v[19] * 20u;
	o = vec4(float(s & 255u) / 255.0, float((s >> 8) & 255u) / 255.0, v_color.b, 1.0);
}
