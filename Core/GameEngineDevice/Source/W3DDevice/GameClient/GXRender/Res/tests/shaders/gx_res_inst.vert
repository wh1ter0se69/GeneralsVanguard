// GeneralsX @test E · rw2-C 11/10/2026 gx_res_test's vertex-input reader: one POINT per instance (or per vertex), at pixel
// (base + index) of a width x height target, carrying the uvec4 record it read through the vertex input unchanged.
#version 450
layout(location = 0) in uvec4 inRecord;
layout(push_constant) uniform PC { uint base; uint width; uint height; uint perVertex; } pc;
layout(location = 0) flat out uvec4 vRecord;
void main()
{
	uint idx = pc.base + (pc.perVertex != 0u ? uint(gl_VertexIndex) : uint(gl_InstanceIndex));
	vec2 px = vec2(float(idx % pc.width) + 0.5, float(idx / pc.width) + 0.5);
	gl_Position = vec4(px / vec2(float(pc.width), float(pc.height)) * 2.0 - 1.0, 0.0, 1.0);
	gl_PointSize = 1.0;
	vRecord = inRecord;
}
