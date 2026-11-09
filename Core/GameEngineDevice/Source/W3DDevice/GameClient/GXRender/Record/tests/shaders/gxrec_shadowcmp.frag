#version 450
// S7 P0-a (D · look-shadowmaps 17/10/2026, PREREG_look-shadowmaps.md Task 1): hardware depth compare through an IMMUTABLE compare
// sampler on a D16 image (the set-2 shape S7 registers). dref 0.5 against a depth image cleared to 0.25 (left) / 0.75 (right).
layout(set = 0, binding = 0) uniform sampler2DShadow uAtlas;
layout(push_constant) uniform Pc { vec2 size; float dref; } pc;
layout(location = 0) out vec4 oColor;
void main()
{
	float v = texture(uAtlas, vec3(gl_FragCoord.xy / pc.size, pc.dref));
	oColor = vec4(v, v, v, 1.0);
}
