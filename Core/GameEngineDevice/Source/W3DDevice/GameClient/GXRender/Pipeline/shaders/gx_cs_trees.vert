#version 450
// gx_cs_trees.vert - hand port of Shaders\Trees.vso (vs.1.1, bytecode sha1 331b76f6472f..., 42 dwords) for the custom-shader
// catalogue (WS-E, rw2-E, 11/10/2026). Pairs with the generic or specialised FF fragment shader (the tree draws set no ps).
//
//   mov r2, v1.wwzw ; add r2, v0, -r2 ; mov a0.x, v1 ; mov r0, c8[a0.x] ; mad r1, r2.zzzw, r0, v0 ; m4x4 oPos, r1, c4
//   mov r2, v1.yyyw ; mul oD0, v2, r2 ; mov oT0, v7 ; add r1, v0, c32 ; mul oT1, r1, c33
//
// Interface = the FF vertex shader's, so the FF fragment shader links unchanged: inputs by GxSemantic location (v0 Position,
// v1 Normal, v2 Color0, v7 Texcoord0: the declaration -> semantic table lives in GxCustomCatalog), outputs at the FF VS's
// locations. The constants c0..c95 alias the VS uniform block at set 0 binding 0 (the FF block is 1888 B; 96 x 16 = 1536 fits).
// Semantics taken from DXVK (dxso_compiler.cpp): a float -> a0 move FLOORS for vs <= 1.1 (:1855); oD0/oD1 are saturated for
// shader model < 3 (:3445); an unwritten output reads 0 (we write every FF output).
invariant gl_Position;

layout(location = 0) in vec4 in_v0;    // GxSemPosition   (decl: FLOAT3)
layout(location = 1) in vec4 in_v1;    // GxSemNormal     (decl: FLOAT3: x = constant index, y = shade, z = sway weight)
layout(location = 4) in vec4 in_v7;    // GxSemTex0       (decl: FLOAT2)
layout(location = 12) in vec4 in_v2;   // GxSemColor0     (decl: D3DCOLOR)

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

void main() {
    vec4 v0 = in_v0, v1 = in_v1, v2 = in_v2, v7 = in_v7;
    vec4 r2 = v1.wwzw;
    r2 = v0 - r2;
    int a0 = int(floor(v1.x));
    vec4 r0 = creg(8 + a0);
    vec4 r1 = r2.zzzw * r0 + v0;
    gl_Position = vec4(dot(r1, c[4]), dot(r1, c[5]), dot(r1, c[6]), dot(r1, c[7]));
#ifdef GX_LOOK_SHADOWMAP
    out_WorldPos = r1.xyz;   // the swayed WORLD position (oPos = r1 x c4..c7, PREREG A9) - taken before r1 is reused below
#endif
#ifdef GX_CS_MUTANT
    gl_Position.x += 0.01;   // TEST-ONLY (the golden's fail arm, never shipped): a planted position change
#endif
    r2 = v1.yyyw;
    out_Color0 = clamp(v2 * r2, 0.0, 1.0);
    out_Texcoord0 = vec4(v7.xy, 0.0, 1.0);   // oT0 = v7: a FLOAT2 attribute's z/w are the d3d defaults (0, 1)
    r1 = v0 + c[32];
    out_Texcoord1 = r1 * c[33];
    out_Normal = vec4(0.0);
    out_Texcoord2 = vec4(0.0); out_Texcoord3 = vec4(0.0); out_Texcoord4 = vec4(0.0);
    out_Texcoord5 = vec4(0.0); out_Texcoord6 = vec4(0.0); out_Texcoord7 = vec4(0.0);
    out_Color1 = vec4(0.0);
    out_Fog = 0.0;
}
