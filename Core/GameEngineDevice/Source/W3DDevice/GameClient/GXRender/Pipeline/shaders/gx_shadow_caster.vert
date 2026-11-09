#version 450
// gx_shadow_caster.vert - S7 GPU sun shadow map (D · look-shadowmaps 17/10/2026, PREREG_look-shadowmaps.md A1/A21, PLAN Task 2):
// the OPAQUE caster replay. A captured caster draw is replayed depth-only into the shadow atlas from the frame's INIT command
// buffer; the device-private pipeline has no fragment stage. Input = the draw's own position stream (float3, the vertex's
// first attribute at the bound offset); lightWorld = World x LightViewProj in D3D element order (_11 _12 ... = row-vector
// v' = v * M), which a GLSL mat4 reads as M transposed, so M^T * v is the row-vector product.
layout(location = 0) in vec3 inPos;
layout(push_constant) uniform Pc { mat4 lightWorld; } pc;
void main()
{
	gl_Position = pc.lightWorld * vec4(inPos, 1.0);
}
