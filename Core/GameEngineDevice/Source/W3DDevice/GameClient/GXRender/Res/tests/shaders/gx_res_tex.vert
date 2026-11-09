// GeneralsX @test E · rw2-C 11/10/2026 gx_res_test's texel reader: one full-target triangle.
#version 450
void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
