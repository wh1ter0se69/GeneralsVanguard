#version 450
// WS-X hybrid spike test mesh: a cube per instance, placed in D3D view space, projected with the game's
// current D3D projection (row-major D3D memory read as a GLSL column-major matrix = its transpose, so
// proj * p reproduces D3D's p * Proj).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inInst;    // xyz = view-space offset, w = scale
layout(location = 2) in vec4 inColour;
layout(push_constant) uniform Push { mat4 proj; vec4 offset; } pc;	// offset: view-space shift per insertion (criterion 2 order test)
layout(location = 0) out vec4 vColour;
void main()
{
	vec3 p = inPos * inInst.w + inInst.xyz + pc.offset.xyz;
	gl_Position = pc.proj * vec4(p, 1.0);
	vColour = inColour;
}
