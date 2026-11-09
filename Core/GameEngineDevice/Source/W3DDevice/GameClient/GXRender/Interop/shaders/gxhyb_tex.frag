#version 450
// WS-X criterion 2 test: the cube shows the texel of a DXVK-owned MANAGED texture read through the export contract.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 vColour;
layout(location = 0) out vec4 outColour;
void main()
{
	outColour = texture(tex, vec2(0.5, 0.5));
}
