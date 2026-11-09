// GeneralsX @test E · rw2-C 11/10/2026 writes the record the vertex stage read (R32G32B32A32_UINT target).
#version 450
layout(location = 0) flat in uvec4 vRecord;
layout(location = 0) out uvec4 outColor;
void main()
{
	outColor = vRecord;
}
