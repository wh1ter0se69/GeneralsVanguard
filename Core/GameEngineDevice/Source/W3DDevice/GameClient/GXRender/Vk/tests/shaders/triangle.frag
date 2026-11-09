#version 450
// GeneralsX @feature E · rw1-B 10/10/2026 WSI window test: flat colour from a push constant.
layout(push_constant) uniform PC { vec4 color; } pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.color; }
