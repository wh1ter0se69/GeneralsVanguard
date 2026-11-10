#version 450
#extension GL_GOOGLE_include_directive : enable
// gx_cs_ps.frag - the custom pixel-shader ports (WS-E, rw2-E, 11/10/2026): ONE module holding every hand-ported ps.1.1 program
// of the game's file shaders (ShadersZH.big / shaders.big), selected per pipeline by specialisation constant 40 (gxCsProgram =
// GxCsProgramId, GxCustomCatalog.h). Built on gx_ff.frag.glsl (DXVK 3.0 adaptation, zlib licence: notice in gx_ff_common.glsl,
// shaders/SOURCE.txt): the varyings, samplers and DXVK's post-PS fog and alpha test are the FF fragment shader's own; only the
// texture-stage cascade is replaced by the program (D3D8 applies fog and the alpha test after a ps as well).
//
// ps.1.x semantics used here: `tex tN` = sample stage N at texture-coordinate set N (the FF VS routes set N through stage N's
// TEXCOORDINDEX and transform, so in_TexcoordN is the right coordinate), divided by w when stage N is PROJECTED; an unbound or
// non-2D stage reads (0,0,0,1) (DXVK 2.6's unbound constant, the classic path). `lrp d,s0,s1,s2` = s0*s1 + (1-s0)*s2 =
// mix(s2,s1,s0) with s0 replicated. Registers hold [0,1] values here (unsigned textures, saturated v0), so no ps.1.x [-1,1]
// clamp can bite; every program below is checked against the classic path on a real frame (WS-E gates), not by this comment.
#define GX_CUSTOM_PS
// v0/v1 are CENTROID-interpolated, as DXVK 2.6 compiles every D3D9 pixel shader (DxsoCompiler::emitDclInterface: a pixel-shader
// Color input is always centroid, references/fbraz3-dxvk dxso_compiler.cpp:638); its FF fragment shader is not, so only this module
// changes (rw3-P, 11/10/2026: under MSAA the centre-interpolated v0 differed on every partially covered pixel of the terrain).
#define GX_COLOR_INTERP centroid
#include "gx_ff.frag.glsl"

layout(constant_id = 40) const uint gxCsProgram = 0u;
#ifdef GX_CS_MUTANT
// TEST-ONLY (the golden's fail arm, never shipped): the program named by constant 41 gets its red channel scaled by 0.97.
layout(constant_id = 41) const uint gxCsMutate = 0u;
#endif

vec4 gxPsTex(uint stage) {
    SamplerState st = getSamplerState(stage);
    vec4 tc = getTexCoord(stage);
    if (st.projected)
        tc /= tc.w;
    vec4 v = vec4(0.0, 0.0, 0.0, 1.0);
    if (st.type == TEXTURE_TYPE_2D)
        v = sample2D(stage, tc.xy);
    return v;
}

// ps.1.x constant c<i> = SetPixelShaderConstant(i). The translator stores c0-c7 in the first 16 bytes of PS-block stage i
// (Constant, Padding, BumpEnvLScale, BumpEnvLOffset - unread by a custom ps, which never runs the stage cascade; the stage's
// BumpEnvMat stays intact for texbem). PS 1.x clamps its float constants to [-1,1] (DXVK dxso_compiler.cpp:1721).
vec4 gxPsConst(uint i) {
    D3D9SharedPSStage s = sharedData.Stages[i];
    return clamp(vec4(uintBitsToFloat(s.Constant), uintBitsToFloat(s.Padding), s.BumpEnvLScale, s.BumpEnvLOffset), -1.0, 1.0);
}

// ps.1.1 `texbem t<stage>, t<m>` (DXVK emitBem): the stage's texture coordinate, projected first when the stage is flagged, then
// offset by the 2x2 bump matrix applied to the .xy of the t<m> result n: u' = tc.x + dot(M.xy, n.xy), v' = tc.y + dot(M.zw, n.xy)
// (GxPackFF stores the matrix 00 10 01 11 in BumpEnvMat). The sample uses the stage's sampler; no second projection.
vec4 gxPsTexBem(uint stage, vec4 n) {
    SamplerState st = getSamplerState(stage);
    vec4 tc = getTexCoord(stage);
    if (st.projected)
        tc /= tc.w;
    vec4 bm = sharedData.Stages[stage].BumpEnvMat;
    tc.x += dot(bm.xy, n.xy);
    tc.y += dot(bm.zw, n.xy);
    vec4 v = vec4(0.0, 0.0, 0.0, 1.0);
    if (st.type == TEXTURE_TYPE_2D)
        v = sample2D(stage, tc.xy);
    return v;
}

vec4 gxCustomPs() {
    vec4 r0 = vec4(0.0), r1 = vec4(0.0);
    switch (gxCsProgram) {
    case 1u:   // fterrain.pso 3c5dff3d7200: tex t0; tex t1; mul r0,t1,t0; mul r0,r0,v0
        r0 = gxPsTex(1u) * gxPsTex(0u);
        r0 *= in_Color0;
        break;
    case 2u:   // fterrain0.pso f404fe8bf81a: tex t1; mov r0,t1; mul r0,r0,v0
        r0 = gxPsTex(1u);
        r0 *= in_Color0;
        break;
    case 3u:   // fterrainnoise.pso e15284e4c197: tex t0,t1,t2; mul r0,t1,t0; mul r0,r0,v0; mul r0,r0,t2
        r0 = gxPsTex(1u) * gxPsTex(0u);
        r0 *= in_Color0;
        r0 *= gxPsTex(2u);
        break;
    case 4u:   // fterrainnoise2.pso 29838a28e055: tex t0..t3; mul r0,t1,t0; mul r0,r0,v0; mul r0,r0,t2; mul r0,r0,t3
        r0 = gxPsTex(1u) * gxPsTex(0u);
        r0 *= in_Color0;
        r0 *= gxPsTex(2u);
        r0 *= gxPsTex(3u);
        break;
    case 5u:   // terrain.pso f2bfcea126a8: tex t0,t1; lrp r0,v0.w,t1,t0; mul r0,r0,v0
        r0 = mix(gxPsTex(0u), gxPsTex(1u), in_Color0.w);
        r0 *= in_Color0;
        break;
    case 6u:   // terrainnoise.pso ceeb171edf12: tex t0,t1,t2; lrp r0,v0.w,t1,t0; mul r0,r0,v0; mul r0,r0,t2
        r0 = mix(gxPsTex(0u), gxPsTex(1u), in_Color0.w);
        r0 *= in_Color0;
        r0 *= gxPsTex(2u);
        break;
    case 7u:   // terrainnoise2.pso 7a208a2813ae: tex t0..t3; lrp r0,v0.w,t1,t0; mul r0,r0,v0; mul r0,r0,t2; mul r0,r0,t3
        r0 = mix(gxPsTex(0u), gxPsTex(1u), in_Color0.w);
        r0 *= in_Color0;
        r0 *= gxPsTex(2u);
        r0 *= gxPsTex(3u);
        break;
    case 8u:   // roadnoise2.pso 96fc9a116458: tex t0,t1,t2; mul r0,t0,t1; mul r0,r0,t2; mul r0,r0,v0
        r0 = gxPsTex(0u) * gxPsTex(1u);
        r0 *= gxPsTex(2u);
        r0 *= in_Color0;
        break;
    case 9u: { // Trees.pso 82aded372c5d: tex t0; mov r0,v0; mul r0,r0,t0; mul_x2 r0.xyz,r0,c1; mov r0.w,t0.w  (dst token 0x81070000: shift 1)
        vec4 t0 = gxPsTex(0u);
        r0 = in_Color0;
        r0 *= t0;
        r0.xyz = (r0.xyz * gxPsConst(1u).xyz) * 2.0;   // mul_x2: the destination shift scales the result by 2^1 (DXVK emitDstStore)
        r0.w = t0.w;
        break;
    }
    case 10u: { // monochrome.pso be96ac175ca9: tex t0; dp3 r1,t0,c0; mul r1,r1,c1; lrp r0,c2,r1,t0
        vec4 t0 = gxPsTex(0u);
        r1 = vec4(dot(t0.xyz, gxPsConst(0u).xyz));   // dp3 writes all four components
        r1 *= gxPsConst(1u);
        r0 = mix(t0, r1, gxPsConst(2u));            // lrp: c2*r1 + (1-c2)*t0, per channel
        break;
    }
    case 11u: { // wave.pso 20c8211bc3cd: tex t0; texbem t1,t0; mul r0,t1,v0
        vec4 t0 = gxPsTex(0u);
        r0 = gxPsTexBem(1u, t0) * in_Color0;
        break;
    }
    case 12u: { // water river 51bf269b9a33 (W3DWater.cpp riverShader): see the source text beside the bytecode there
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u), t2 = gxPsTex(2u), t3 = gxPsTex(3u);
        r0.rgb = in_Color0.rgb * t0.rgb;            // mul r0.rgb, v0, t0
        r0.a = t0.a;                                 // mov r0.a, t0
        r1 = t1 * t2;                                // mul r1, t1, t2
        r1.rgb += t3.rgb;                            // add r1.rgb, r1, t3
        r1.rgb *= in_Color0.a;                       // mul r1.rgb, r1, v0.a
        r0.a = r0.a * t3.a;                          // +mul r0.a, r0, t3  (co-issued alpha op)
        r0.rgb += r1.rgb;                            // add r0.rgb, r0, r1
        break;
    }
    case 13u: { // water (texbem) d689945b5f3c: tex t0; tex t1; texbem t2,t1; mul r0,v0,t0; mul r1.rgb,t2,c0; add r0.rgb,r0,r1
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u);
        vec4 t2 = gxPsTexBem(2u, t1);
        r0 = in_Color0 * t0;
        r1.rgb = t2.rgb * gxPsConst(0u).rgb;
        r0.rgb += r1.rgb;
        break;
    }
    case 14u: { // water trapezoid d85492a785b6: tex t0..t3; mul r0,v0,t0; mad r0.rgb,t1,t2,r0; mul r0.rgb,r0,t3
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u), t2 = gxPsTex(2u), t3 = gxPsTex(3u);
        r0 = in_Color0 * t0;
        r0.rgb = t1.rgb * t2.rgb + r0.rgb;
        r0.rgb *= t3.rgb;
        break;
    }
    // Live water (GameClient/GXWaterLive.h): the source text is beside each array there. No
    // register clamp between instructions, as DXVK compiles ps.1.x (dxso_compiler.h emitDstStore: _x4 multiplies, only _sat clamps), so
    // the glints' squared x4 peaks may exceed 1 until the constants scale them back; the render target clamps the output, as classic's.
    case 16u: { // water:glare 29f8fbf7ffd0: tex t1..t3; mul r1,t1,t2; mul_x4 r1,r1,r1; mad r1.rgb,t2,c2,r1; mul r1.rgb,r1,c1; mul r0.rgb,r1,t3; +mov r0.a,c1
        vec4 t1 = gxPsTex(1u), t2 = gxPsTex(2u), t3 = gxPsTex(3u);
        r1 = t1 * t2;                                // mul r1, t1, t2
        r1 = (r1 * r1) * 4.0;                        // mul_x4 r1, r1, r1  (dst token 0x820f0001: shift 2)
        r1.rgb = t2.rgb * gxPsConst(2u).rgb + r1.rgb;   // mad r1.rgb, t2, c2, r1
        r1.rgb *= gxPsConst(1u).rgb;                 // mul r1.rgb, r1, c1
        r0.rgb = r1.rgb * t3.rgb;                    // mul r0.rgb, r1, t3
        r0.a = gxPsConst(1u).a;                      // +mov r0.a, c1  (co-issued alpha op)
        break;
    }
    case 17u: { // water:foam 7dd0bb74c3bd: tex t0..t3; mad r1,t1,c1,c2; add r1,r1,t2; mul r0.rgb,v0,t3; +mul r0.a,t0,r1.b; mul r0.a,r0,v0
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u), t2 = gxPsTex(2u), t3 = gxPsTex(3u);
        r1 = t1 * gxPsConst(1u) + gxPsConst(2u);     // mad r1, t1, c1, c2
        r1 += t2;                                    // add r1, r1, t2
        r0.rgb = in_Color0.rgb * t3.rgb;             // mul r0.rgb, v0, t3
        r0.a = t0.a * r1.b;                          // +mul r0.a, t0, r1.b  (source token 0x80aa0001: blue replicated)
        r0.a = r0.a * in_Color0.a;                   // mul r0.a, r0, v0
        break;
    }
    // The remastered ground (TerrainTex.cpp gxgEnsureDevice, R-G1, the same session): the source text is beside each array there. lrp
    // is written as DXVK emits it (emitMix: s0 * (s1 - s2) + s2, a mul and an add), not GLSL mix(); `_bias` = x - 0.5 (DXVK
    // emitSrcOperandPostSwizzleModifiers); `_x4_sat` / `_x2_sat` scale first, then clamp to [0,1] (emitDstStore).
    case 18u: { // gx:ground-base c0b073937f4d: tex t0..t3; sub r1.a,t3,t2; mad r1.a,r1,c0,v0_bias; mad_x4_sat r1.a,r1,c1,c2;
                // lrp r0,r1.a,t1,t0; lrp r1.rgb,r1.a,t3,t2; lrp r1.rgb,c3,r1,c4; mul_x2_sat r1.rgb,r1,v0; mul r0.rgb,r0,r1
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u), t2 = gxPsTex(2u), t3 = gxPsTex(3u);
        r1.a = t3.a - t2.a;                                                   // sub r1.a, t3, t2
        r1.a = r1.a * gxPsConst(0u).a + (in_Color0.a - 0.5);                  // mad r1.a, r1, c0, v0_bias
        r1.a = clamp((r1.a * gxPsConst(1u).a + gxPsConst(2u).a) * 4.0, 0.0, 1.0);   // mad_x4_sat r1.a, r1, c1, c2 (dst 0x82180001)
        r0 = r1.a * (t1 - t0) + t0;                                           // lrp r0, r1.a, t1, t0
        r1.rgb = r1.a * (t3.rgb - t2.rgb) + t2.rgb;                           // lrp r1.rgb, r1.a, t3, t2
        r1.rgb = gxPsConst(3u).rgb * (r1.rgb - gxPsConst(4u).rgb) + gxPsConst(4u).rgb;   // lrp r1.rgb, c3, r1, c4
        r1.rgb = clamp((r1.rgb * in_Color0.rgb) * 2.0, 0.0, 1.0);             // mul_x2_sat r1.rgb, r1, v0 (dst 0x81170001)
        r0.rgb *= r1.rgb;                                                     // mul r0.rgb, r0, r1
        break;
    }
    case 19u: { // gx:ground-mult ff56cbe5042d: tex t0..t3; lrp r1,c0,t1,c1; mul r0,t0,r1; lrp r1,c2,t2,c3; mul r0,r0,r1; lrp r1,c4,t3,c3; mul_x2 r0,r0,r1
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u), t2 = gxPsTex(2u), t3 = gxPsTex(3u);
        r1 = gxPsConst(0u) * (t1 - gxPsConst(1u)) + gxPsConst(1u);           // lrp r1, c0, t1, c1
        r0 = t0 * r1;                                                         // mul r0, t0, r1
        r1 = gxPsConst(2u) * (t2 - gxPsConst(3u)) + gxPsConst(3u);           // lrp r1, c2, t2, c3
        r0 *= r1;                                                             // mul r0, r0, r1
        r1 = gxPsConst(4u) * (t3 - gxPsConst(3u)) + gxPsConst(3u);           // lrp r1, c4, t3, c3
        r0 = (r0 * r1) * 2.0;                                                 // mul_x2 r0, r0, r1 (dst 0x810f0000)
        break;
    }
    case 20u: { // gx:ground-mult-lite f14805975340: tex t0, t1; lrp r1,c0,t1,c1; mul r0,t0,r1
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u);
        r1 = gxPsConst(0u) * (t1 - gxPsConst(1u)) + gxPsConst(1u);           // lrp r1, c0, t1, c1
        r0 = t0 * r1;                                                         // mul r0, t0, r1
        break;
    }
    // RA3 water's sheen (GameClient/GXWaterRA3.h): the source text is beside the array there. As DXVK
    // compiles it: `_bx2` = fma(x, 2, -1) (emitSrcOperandPostSwizzleModifiers), the add then `_d2` = x 0.5 (emitDstStore), dp3 = the
    // three products summed in order (emitDot), `_sat` clamps to [0,1], mad = a mul then an add (emitMad); c0/c1 clamped to [-1,1].
    case 21u: { // water:ra3 fe4234069f19: tex t0,t1,t3; add_d2 r1,t0_bx2,t1_bx2; dp3_sat r0,r1,c0; mul r0,r0,r0 (x3); mad r0.rgb,r0,c1,v0; mul r0.rgb,r0,t3
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u), t3 = gxPsTex(3u);
        r1 = ((t0 * 2.0 - 1.0) + (t1 * 2.0 - 1.0)) * 0.5;       // add_d2 r1, t0_bx2, t1_bx2 (dst 0x8f0f0001: shift -1)
        vec4 c0 = gxPsConst(0u);
        float d = r1.x * c0.x + r1.y * c0.y;
        d = d + r1.z * c0.z;                                      // dp3 r0, r1, c0
        r0 = vec4(clamp(d, 0.0, 1.0));                            // _sat (dst 0x801f0000), replicated to all four
        r0 = r0 * r0;                                             // mul r0, r0, r0  ^2
        r0 = r0 * r0;                                             // ^4
        r0 = r0 * r0;                                             // ^8
        r0.rgb = r0.rgb * gxPsConst(1u).rgb + in_Color0.rgb;     // mad r0.rgb, r0, c1, v0
        r0.rgb *= t3.rgb;                                         // mul r0.rgb, r0, t3
        break;
    }
    // RA3 water's sheen on RIVERS (the same header): case 21's program with t3 swapped for v0.a - the
    // bank fade x the shroud per vertex. Compiled exactly as case 21; the last instruction reads the replicated vertex alpha.
    case 22u: { // water:ra3river 383ecb43fb6a: tex t0,t1; add_d2 r1,t0_bx2,t1_bx2; dp3_sat r0,r1,c0; mul r0,r0,r0 (x3); mad r0.rgb,r0,c1,v0; mul r0.rgb,r0,v0.a
        vec4 t0 = gxPsTex(0u), t1 = gxPsTex(1u);
        r1 = ((t0 * 2.0 - 1.0) + (t1 * 2.0 - 1.0)) * 0.5;       // add_d2 r1, t0_bx2, t1_bx2
        vec4 c0 = gxPsConst(0u);
        float d = r1.x * c0.x + r1.y * c0.y;
        d = d + r1.z * c0.z;                                      // dp3 r0, r1, c0
        r0 = vec4(clamp(d, 0.0, 1.0));                            // _sat, replicated to all four
        r0 = r0 * r0;                                             // mul r0, r0, r0  ^2
        r0 = r0 * r0;                                             // ^4
        r0 = r0 * r0;                                             // ^8
        r0.rgb = r0.rgb * gxPsConst(1u).rgb + in_Color0.rgb;     // mad r0.rgb, r0, c1, v0
        r0.rgb *= in_Color0.a;                                    // mul r0.rgb, r0, v0.a
        break;
    }
    default:   // an unknown program id: loud magenta, never silently the FF result (the catalogue never builds this)
        r0 = vec4(1.0, 0.0, 1.0, 1.0);
        break;
    }
#ifdef GX_CS_MUTANT
    if (gxCsProgram == gxCsMutate)
        r0.r *= 0.97;
#endif
    return r0;
}
