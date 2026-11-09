#version 450
// WS-X criterion 3 test: the depth-EQUAL second pass paints white where it passes.
layout(location = 0) in vec4 vColour;
layout(location = 0) out vec4 outColour;
void main()
{
	outColour = vec4(1.0);
}
