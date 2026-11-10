#version 450
// GeneralsX: gx_record_test's fragment shader: the vertex colour, or texture stage 0.
layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(push_constant) uniform PC { vec2 invHalfExtent; float sampleTex; float pad; } pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.sampleTex > 0.5 ? texture(tex0, inUv) : inColor; }
