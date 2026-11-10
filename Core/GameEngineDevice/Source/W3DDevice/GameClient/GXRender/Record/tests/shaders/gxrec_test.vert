#version 450
// GeneralsX: gx_record_test's pixel-exact vertex shader. Positions are PIXEL coordinates (x, y)
// + depth z; the push block maps them to NDC. inOffset (location 3) is an INSTANCE-rate pixel offset, zero from the null
// stream when the pipeline has no instance binding.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inOffset;
layout(set = 0, binding = 0) uniform VsBlock { vec4 colorAdd; } vb;   // the writer's FF VS block slot (dynamic UBO)
layout(push_constant) uniform PC { vec2 invHalfExtent; float sampleTex; float pad; } pc;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;
void main() {
	gl_Position = vec4((inPos.xy + inOffset.xy) * pc.invHalfExtent - 1.0, inPos.z, 1.0);
	gl_PointSize = 1.0;
	outColor = inColor + vb.colorAdd + vec4(0.0, 0.0, 0.0, inOffset.w);
	outUv = inUv;
}
