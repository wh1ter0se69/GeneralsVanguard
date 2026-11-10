#version 450
// gx_cs_shadow.vert - hand port of the GX_SHADOW_GPU shadow-volume vertex shader (vs.1.1, GxShadowVolumeVS.h kGxShadowVS, catalogue
// 'gx:shadowvol', bytecode sha1 84d63968db0f..., 87 dwords) for the custom-shader catalogue. Both stencil
// passes draw it with colour writes off; no ps is bound, so it pairs with the generic FF fragment shader.
//
//   mov a0.x, v3.x ; mov r1.w, c0.y ; mov r2.w, c0.y ; sub r0.xyz, v2, c3[a0.x] ; dp3 r0.x, r0, v1 ; slt r0.y, r0.x, c0.x
//   sub r0.z, c0.y, r0.y ; sub r1.xyz, v0, c3[a0.x] ; mul r1.xyz, r1, c3[a0.x].w ; mad r1.xyz, r1, r0.z, v0
//   dp4 r2.x/y/z, r1, c0/c1/c2[a0.x] ; dp4 r3.x/y/z/w, r2, c1/c2/c3/c4 ; dp4 oPos.x/y/z/w, r3, c5/c6/c7/c8 ; mov oD0, c0.y
//
// Interface and conventions as gx_cs_trees.vert: inputs by GxSemantic location (v0 Position FLOAT3, v1 Normal FLOAT3, v2 Tex0 FLOAT3,
// v3 Tex1 FLOAT1 - GxCustomCatalog.cpp GxCsVertexInputs), outputs at gx_ff.vert's locations, constants c0..c95 alias the VS block
// (the native device reports 96 vertex-shader constants, so the game's windows reach c92 at most). Arithmetic in DXVK 2.6's dxso
// shapes (references/fbraz3-dxvk src/dxso/dxso_compiler.cpp), not GLSL's: dp3/dp4 = per-component products summed left to right
// (emitDot), mad = a mul then an add with no fma emitted (emitMad; any contraction is the driver's, for DXVK's SPIR-V too), slt = an
// ordered less-than selecting 1 or 0, a float -> a0 move floors
// (vs <= 1.1). Not modelled: d3d9.floatEmulation "strict" (drivers with mulz, e.g. NVIDIA >= 565.57.1) makes a product 0 when either
// operand is 0 - it differs from a plain multiply only for a non-finite operand or the sign of a zero.
invariant gl_Position;

layout(location = 0) in vec4 in_v0;    // GxSemPosition (decl: FLOAT3) corner position
layout(location = 1) in vec4 in_v1;    // GxSemNormal   (decl: FLOAT3) its face's normal
layout(location = 4) in vec4 in_v2;    // GxSemTex0     (decl: FLOAT3) its face's poly[0] vertex
layout(location = 5) in vec4 in_v3;    // GxSemTex1     (decl: FLOAT1) the copy's first constant register

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

vec4 creg(int i) { return (i >= 0 && i < 96) ? c[i] : vec4(0.0); }   // an out-of-range constant reads 0 (d3d)
float dp3(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float dp4(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

void main() {
    vec4 v0 = in_v0, v1 = in_v1, v2 = in_v2, v3 = in_v3;
    int a0 = int(floor(v3.x));                    // mov a0.x, v3.x
    vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3;
    r1.w = c[0].y;                                // mov r1.w, c0.y
    r2.w = c[0].y;                                // mov r2.w, c0.y
    vec4 l = creg(a0 + 3);                        // c3[a0.x] = (L, s)
    r0.xyz = v2.xyz - l.xyz;                      // sub r0.xyz, v2, c3[a0.x]
    r0.x = dp3(r0, v1);                           // dp3 r0.x, r0, v1
    r0.y = r0.x < c[0].x ? 1.0 : 0.0;             // slt r0.y, r0.x, c0.x
    r0.z = c[0].y - r0.y;                         // sub r0.z, c0.y, r0.y
    r1.xyz = v0.xyz - l.xyz;                      // sub r1.xyz, v0, c3[a0.x]
    r1.xyz = r1.xyz * l.www;                      // mul r1.xyz, r1, c3[a0.x].w
    r1.xyz = r1.xyz * r0.zzz;                     // mad r1.xyz, r1, r0.z, v0 (the mul ...
    r1.xyz = r1.xyz + v0.xyz;                     //  ... then the add)
    r2.x = dp4(r1, creg(a0 + 0));                 // dp4 r2.x, r1, c0[a0.x]
    r2.y = dp4(r1, creg(a0 + 1));                 // dp4 r2.y, r1, c1[a0.x]
    r2.z = dp4(r1, creg(a0 + 2));                 // dp4 r2.z, r1, c2[a0.x]
    r3 = vec4(dp4(r2, c[1]), dp4(r2, c[2]), dp4(r2, c[3]), dp4(r2, c[4]));
    gl_Position = vec4(dp4(r3, c[5]), dp4(r3, c[6]), dp4(r3, c[7]), dp4(r3, c[8]));
#ifdef GX_CS_MUTANT
    gl_Position.x += 0.01;   // TEST-ONLY (the golden's fail arm, never shipped): a planted position change
#endif
    out_Color0 = clamp(vec4(c[0].y), 0.0, 1.0);   // mov oD0, c0.y (oD0 saturates for shader model < 3)
    out_Normal = vec4(0.0);
    out_Texcoord0 = vec4(0.0); out_Texcoord1 = vec4(0.0); out_Texcoord2 = vec4(0.0); out_Texcoord3 = vec4(0.0);
    out_Texcoord4 = vec4(0.0); out_Texcoord5 = vec4(0.0); out_Texcoord6 = vec4(0.0); out_Texcoord7 = vec4(0.0);
    out_Color1 = vec4(0.0);
    out_Fog = 0.0;
#ifdef GX_LOOK_SHADOWMAP
    out_WorldPos = vec3(0.0);   // never a receiver (after the stencil-shadow boundary / colour writes off): written for the interface only
#endif
}
