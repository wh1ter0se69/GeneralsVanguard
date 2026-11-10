// GeneralsX: writes the texel the sampled view holds at this pixel (texelFetch: no filtering, no
// coordinate rounding), through the view's format and swizzle - what a draw of the texture would sample.
#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 outColor;
void main()
{
	outColor = texelFetch(tex, ivec2(gl_FragCoord.xy), 0);
}
