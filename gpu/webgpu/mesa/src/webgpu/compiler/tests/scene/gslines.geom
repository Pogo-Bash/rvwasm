#version 450
// wide lines, as Blender's overlays draw them: each line becomes a quad (a 4-vertex triangle strip)
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 col[];
layout(location = 1) in float w[];
layout(location = 0) out vec4 fcol;
layout(location = 1) noperspective out vec2 edge;
void main()
{
	vec2 p0 = gl_in[0].gl_Position.xy, p1 = gl_in[1].gl_Position.xy;
	vec2 n = normalize(vec2(p0.y - p1.y, p1.x - p0.x));
	for (int k = 0; k < 4; k++) {
		int v = k >> 1;
		float s = (k & 1) == 0 ? -1.0 : 1.0;
		vec2 p = (v == 0 ? p0 : p1) + n * s * w[v];
		gl_Position = vec4(p, 0.5, 1.0);
		fcol = col[v];
		edge = vec2(s, float(v));
		EmitVertex();
	}
	EndPrimitive();
}
