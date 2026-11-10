#version 450
// gx_cs_wave.vert - hand port of Shaders\wave.vso (vs.1.1, bytecode sha1 9cc6bd1df897..., 39 dwords) for the custom-shader catalogue
// (WS-E, rw2-E, 11/10/2026). Pairs with gx_cs_ps.frag program 11 (wave.pso) in the water pass.
//
//   dp4 r0.x,v0,c2 ; dp4 r0.y,v0,c3 ; dp4 r0.z,v0,c4 ; dp4 r0.w,v0,c5 ; mov oPos,r0 ; rcp r1.w,r0.w ; mul r1.xy,r0.xyyy,r1.w
//   mad oT1.xy,r1.xyyy,c6.xyyy,c6.zwww ; mov oT0,v2 ; mov oD0,v1
//
// Interface and conventions as gx_cs_trees.vert: inputs by GxSemantic location (v0 Position FLOAT3, v1 Color0 D3DCOLOR, v2 Tex0
// FLOAT2 - W3DWater.cpp:919), outputs at the FF vertex shader's locations, constants c0..c95 alias the VS block, oD0 saturated.
// oT1.zw are never written by the shader: DXVK's unwritten output components are 0.
invariant gl_Position;

layout(location = 0) in vec4 in_v0;
layout(location = 4) in vec4 in_v2;
layout(location = 12) in vec4 in_v1;

layout(location = 0) out vec4 out_Normal;
layout(location = 1) out vec4 out_Texcoord0;
layout(location = 2) out vec4 out_Texcoord1;
layout(location = 3) out vec4 out_Texcoord2;
layout(location = 4) out vec4 out_Texcoord3;
layout(location = 5) out vec4 out_Texcoord4;
layout(location = 6) out vec4 out_Texcoord5;
layout(location = 7) out vec4 out_Texcoord6;
layout(location = 8) out vec4 out_Texcoord7;
layout(location = 9) out vec4 out_Color0;
layout(location = 10) out vec4 out_Color1;
layout(location = 11) out float out_Fog;
#ifdef GX_LOOK_SHADOWMAP
// GX S7 (A20/A36, PLAN P2): the key-on family (regen.sh "@SM") writes the WORLD position the
// receiver's shadow-map lookup needs (the key-on fragment shader reads location 12; every vertex shader it pairs with writes it).
layout(location = 12) out vec3 out_WorldPos;
#endif

layout(set = 0, binding = 0, std140) uniform GxCsVs { vec4 c[96]; };

void main() {
    vec4 r0;
    r0.x = dot(in_v0, c[2]);
    r0.y = dot(in_v0, c[3]);
    r0.z = dot(in_v0, c[4]);
    r0.w = dot(in_v0, c[5]);
    gl_Position = r0;
#ifdef GX_CS_MUTANT
    gl_Position.x += 0.01;   // TEST-ONLY (the golden's fail arm, never shipped): a planted position change
#endif
    float rw = 1.0 / r0.w;                                  // rcp r1.w, r0.w
    vec2 r1xy = r0.xy * rw;                                 // mul r1.xy, r0.xyyy, r1.w
    out_Texcoord1 = vec4(r1xy * c[6].xy + c[6].zw, 0.0, 0.0);   // mad oT1.xy, r1.xyyy, c6.xyyy, c6.zwww
    out_Texcoord0 = in_v2;                                  // mov oT0, v2   (a FLOAT2 attribute reads (u, v, 0, 1))
    out_Color0 = clamp(in_v1, 0.0, 1.0);                    // mov oD0, v1   (oD0 saturates for shader model < 3)
    out_Normal = vec4(0.0);
    out_Texcoord2 = vec4(0.0); out_Texcoord3 = vec4(0.0); out_Texcoord4 = vec4(0.0);
    out_Texcoord5 = vec4(0.0); out_Texcoord6 = vec4(0.0); out_Texcoord7 = vec4(0.0);
    out_Color1 = vec4(0.0);
    out_Fog = 0.0;
#ifdef GX_LOOK_SHADOWMAP
    out_WorldPos = vec3(0.0);   // never a receiver (after the stencil-shadow boundary / colour writes off): written for the interface only
#endif
}
