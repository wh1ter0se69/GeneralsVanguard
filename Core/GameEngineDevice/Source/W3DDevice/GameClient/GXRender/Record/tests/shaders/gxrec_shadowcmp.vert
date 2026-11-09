#version 450
// S7 P0-a (D · look-shadowmaps 17/10/2026, PREREG_look-shadowmaps.md Task 1): one full-screen triangle, no inputs.
void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
