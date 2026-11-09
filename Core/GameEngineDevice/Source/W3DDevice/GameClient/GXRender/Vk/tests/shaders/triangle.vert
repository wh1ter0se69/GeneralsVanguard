#version 450
// GeneralsX @feature E · rw1-B 10/10/2026 WSI window test: one triangle, positions in the shader (no vertex buffer).
const vec2 kPos[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
void main() { gl_Position = vec4(kPos[gl_VertexIndex], 0.0, 1.0); }
