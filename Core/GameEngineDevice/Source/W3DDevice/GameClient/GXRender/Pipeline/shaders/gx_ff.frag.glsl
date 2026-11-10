// gx_ff.frag.glsl - the fixed-function fragment ubershader's body (WS-D week 2, C · rw2-D, 11/10/2026), included by
// gx_ff.frag (smooth colours), gx_ff_flat.frag (D3DSHADE_FLAT) and gx_ff_mutant.frag (the identity gate's fail arm only).
// ADAPTED from DXVK 3.0 src/d3d9/shaders/d3d9_fixed_function_frag.glsl (tag v3.0, commit 97fe0c66f; zlib/libpng licence,
// notice in gx_ff_common.glsl and shaders/SOURCE.txt). ALTERED by GeneralsX (marked "GX:"): set 0 binding 1 std140 uniform
// block GxFFPs (GxFFState.h GxFFPsBlock) replaces the shared-data buffer and the push data; textures are eight 2D combined
// image samplers at set 1 bindings 0-7 (cube and volume samplers removed: GxPackFF refuses them; depth-compare removed: D3D8
// FF never samples a depth texture); an unbound stage texture reads (0,0,0,1) as in DXVK 2.6, the classic path; colour
// interpolation chosen by GX_COLOR_INTERP (DXVK patches flat shading in at pipeline creation); `demote` -> `discard` (the
// alpha test is the last statement, no derivative follows it); the shadowed previousStageColorOp of sampleTexture fixed
// (DXVK 3.0 never applied the BUMPENVMAPLUMINANCE scale); GX_FF_MUTANT plants one arithmetic change for the fail arm.
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_spirv_intrinsics : require
#extension GL_EXT_control_flow_attributes : require

#include "gx_ff_common.glsl"

// The locations need to match with RegisterLinkerSlot in dxso_util.cpp
#ifndef INTERP_MODE
#define INTERP_MODE
#endif
#ifndef GX_COLOR_INTERP
#define GX_COLOR_INTERP INTERP_MODE
#endif

layout(location = 0) INTERP_MODE in vec4 in_Normal;
layout(location = 1) INTERP_MODE in vec4 in_Texcoord0;
layout(location = 2) INTERP_MODE in vec4 in_Texcoord1;
layout(location = 3) INTERP_MODE in vec4 in_Texcoord2;
layout(location = 4) INTERP_MODE in vec4 in_Texcoord3;
layout(location = 5) INTERP_MODE in vec4 in_Texcoord4;
layout(location = 6) INTERP_MODE in vec4 in_Texcoord5;
layout(location = 7) INTERP_MODE in vec4 in_Texcoord6;
layout(location = 8) INTERP_MODE in vec4 in_Texcoord7;
layout(location = 9) GX_COLOR_INTERP in vec4 in_Color0;
layout(location = 10) GX_COLOR_INTERP in vec4 in_Color1;
layout(location = 11) INTERP_MODE in float in_Fog;

layout(location = 0) out vec4 out_Color0;

// Alpha test parameters
const uint VK_COMPARE_OP_NEVER             = 0;
const uint VK_COMPARE_OP_LESS              = 1;
const uint VK_COMPARE_OP_EQUAL             = 2;
const uint VK_COMPARE_OP_LESS_OR_EQUAL     = 3;
const uint VK_COMPARE_OP_GREATER           = 4;
const uint VK_COMPARE_OP_NOT_EQUAL         = 5;
const uint VK_COMPARE_OP_GREATER_OR_EQUAL  = 6;
const uint VK_COMPARE_OP_ALWAYS            = 7;

struct AlphaTestState {
    uint compareOp;
    uint precisionBits;
};

AlphaTestState getAlphaTestState() {
    AlphaTestState result;
    result.compareOp = bitfieldExtract(alphaTestAndModeArgs, 0, 3);
    result.precisionBits = bitfieldExtract(alphaTestAndModeArgs, 4, 4);
    return result;
}

// Sampler state
const uint TEXTURE_TYPE_2D         = 0;
const uint TEXTURE_TYPE_3D         = 1;
const uint TEXTURE_TYPE_CUBE       = 2;
const uint TEXTURE_TYPE_NULL       = 3;

struct SamplerState {
    bool projected;
    uint type;
};

SamplerState getSamplerState(uint idx) {
    SamplerState result;
    result.projected = bitfieldExtract(packedProjMaskAndFfArgs, int(idx), 1) != 0;
    result.type = bitfieldExtract(psSamplerTypes, int(idx + idx), 2);
    return result;
}

// Fixed-function texture stage parameters
const uint D3DTOP_DISABLE                   = 1;
const uint D3DTOP_SELECTARG1                = 2;
const uint D3DTOP_SELECTARG2                = 3;
const uint D3DTOP_MODULATE                  = 4;
const uint D3DTOP_MODULATE2X                = 5;
const uint D3DTOP_MODULATE4X                = 6;
const uint D3DTOP_ADD                       = 7;
const uint D3DTOP_ADDSIGNED                 = 8;
const uint D3DTOP_ADDSIGNED2X               = 9;
const uint D3DTOP_SUBTRACT                  = 10;
const uint D3DTOP_ADDSMOOTH                 = 11;
const uint D3DTOP_BLENDDIFFUSEALPHA         = 12;
const uint D3DTOP_BLENDTEXTUREALPHA         = 13;
const uint D3DTOP_BLENDFACTORALPHA          = 14;
const uint D3DTOP_BLENDTEXTUREALPHAPM       = 15;
const uint D3DTOP_BLENDCURRENTALPHA         = 16;
const uint D3DTOP_PREMODULATE               = 17;
const uint D3DTOP_MODULATEALPHA_ADDCOLOR    = 18;
const uint D3DTOP_MODULATECOLOR_ADDALPHA    = 19;
const uint D3DTOP_MODULATEINVALPHA_ADDCOLOR = 20;
const uint D3DTOP_MODULATEINVCOLOR_ADDALPHA = 21;
const uint D3DTOP_BUMPENVMAP                = 22;
const uint D3DTOP_BUMPENVMAPLUMINANCE       = 23;
const uint D3DTOP_DOTPRODUCT3               = 24;
const uint D3DTOP_MULTIPLYADD               = 25;
const uint D3DTOP_LERP                      = 26;

// These differ from the actual D3D definitions. DXVK will shift
// the modifiers one to the right since only 7 args are defined.
const uint D3DTA_SELECTMASK                 = 0x07;

const uint D3DTA_DIFFUSE                    = 0x00;
const uint D3DTA_CURRENT                    = 0x01;
const uint D3DTA_TEXTURE                    = 0x02;
const uint D3DTA_TFACTOR                    = 0x03;
const uint D3DTA_SPECULAR                   = 0x04;
const uint D3DTA_TEMP                       = 0x05;
const uint D3DTA_CONSTANT                   = 0x06;

const uint D3DTA_COMPLEMENT                 = 0x08;
const uint D3DTA_ALPHAREPLICATE             = 0x10;

struct TextureStageArguments {
    uint arg0;
    uint arg1;
    uint arg2;
};

struct TextureStage {
    bool storeToTemp;
    uint colorOp;
    uint alphaOp;
    TextureStageArguments colorArgs;
    TextureStageArguments alphaArgs;
};

TextureStage getTextureStage(uint idx) {
    uint ops = 0u;
    uint args = 0u;

    switch (idx) {
        case 0: ops = packedStageOps01 >>  0; args = packedStageArgs0; break;
        case 1: ops = packedStageOps01 >> 16; args = packedStageArgs1; break;
        case 2: ops = packedStageOps23 >>  0; args = packedStageArgs2; break;
        case 3: ops = packedStageOps23 >> 16; args = packedStageArgs3; break;
        case 4: ops = packedStageOps45 >>  0; args = packedStageArgs4; break;
        case 5: ops = packedStageOps45 >> 16; args = packedStageArgs5; break;
        case 6: ops = packedStageOps67 >>  0; args = packedStageArgs6; break;
        case 7: ops = packedStageOps67 >> 16; args = packedStageArgs7; break;
    }

    TextureStage result;
    result.storeToTemp = bitfieldExtract(ops, 15, 1) != 0;
    result.colorOp = bitfieldExtract(ops, 0, 5);
    result.alphaOp = bitfieldExtract(ops, 8, 5);
    result.colorArgs.arg0 = bitfieldExtract(args,  0, 5);
    result.colorArgs.arg1 = bitfieldExtract(args,  5, 5);
    result.colorArgs.arg2 = bitfieldExtract(args, 10, 5);
    result.alphaArgs.arg0 = bitfieldExtract(args, 16, 5);
    result.alphaArgs.arg1 = bitfieldExtract(args, 21, 5);
    result.alphaArgs.arg2 = bitfieldExtract(args, 26, 5);
    return result;
}


// Checks whether point sprites are enabled
bool isPointSpriteEnabled() {
    return bitfieldExtract(alphaTestAndModeArgs, 8, 1) != 0;
}

// GX: = GxFFState.h GxFFPsStage / GxFFPsBlock (std140: BumpEnvMat is one vec4 = 00 10 01 11, i.e. DXVK's BumpEnvMat[0] then [1]).
struct D3D9SharedPSStage {
    uint Constant;
    uint Padding;
    float BumpEnvLScale;
    float BumpEnvLOffset;
    vec4 BumpEnvMat;
};

layout(set = 0, binding = 1, std140)   // GX: was set = CBV_SET, binding = CBV_PS_SHARED, scalar + the push data
uniform SharedData {
    D3D9SharedPSStage Stages[TextureStageCount];
    GxFFFog fog;
    uint textureFactor;
#if defined(GX_LOOK_SHADOWMAP)
    uint drawBits;   // GX S7 (A36): GxFFState.h GxFFPsBlock::DrawBits = pad0[0] (offset 276); readers that add a define add it to this #if
    float ownScale;  // GX S7 A41: GxFFPsBlock::OwnScale (offset 280) - a clamped instance's own light, read only with DrawBits bit 3
    float ownZ0;     // GX S7 A41: GxFFPsBlock::OwnZ0 (offset 284)
#endif
} sharedData;

// GX: eight 2D combined image samplers (DXVK: texture arrays + a sampler heap indexed from push data).
layout(set = 1, binding = 0) uniform sampler2D gxTex0;
layout(set = 1, binding = 1) uniform sampler2D gxTex1;
layout(set = 1, binding = 2) uniform sampler2D gxTex2;
layout(set = 1, binding = 3) uniform sampler2D gxTex3;
layout(set = 1, binding = 4) uniform sampler2D gxTex4;
layout(set = 1, binding = 5) uniform sampler2D gxTex5;
layout(set = 1, binding = 6) uniform sampler2D gxTex6;
layout(set = 1, binding = 7) uniform sampler2D gxTex7;

#ifdef GX_LOOK_SHADOWMAP
// GX S7 GPU sun shadow map (D · look-shadowmaps-2 17/10/2026; PREREG_look-shadowmaps A2/A3/A10/A14/A26/A36, PLAN P2): the RECEIVER.
// Set 2 = the look frame set (GxRecordLayout, mask bit 0): b0 the D16 atlas through an immutable compare sampler (LESS_OR_EQUAL,
// LINEAR, clamp: P0-a's shape, A35), b1 the same atlas with an immutable NEAREST sampler (raw depths for the blocker search, A10), b2 the
// frame's UBO (Rec::GxLookFrameUbo, GxLookFrame.h - the same fit, rects and step the frame's INIT replay used).
layout(location = 12) INTERP_MODE in vec3 in_WorldPos;
layout(set = 2, binding = 0) uniform sampler2DShadow gxSmCmp;
layout(set = 2, binding = 1) uniform sampler2D gxSmRaw;
layout(set = 2, binding = 2, std140) uniform GxLookFrame {
    mat4 lightVP[2];     // per cascade: world -> clip, D3D element order read column-major, so lightVP * p is the row-vector product
    vec4 uvMap[2];       // clip xy -> atlas uv: uv = clip.xy * uvMap.xy + uvMap.zw (the replay's viewport is the cascade rect)
    vec4 uvClamp[2];     // the cascade rect in uv, inset half a texel: every tap stays inside its own cascade
    vec4 shadowColor;    // the map's shadow colour, rgba 0..1
    vec4 limits;         // x/y: c0/c1's usable clip half-extent (the guard band removed), z: the c0 -> c1 blend band, w: Rmax (texels)
    vec4 texel;          // xy: one atlas texel in uv, z/w: c0/c1's texel - TANGENT units on a perspective pass (A39: x the axis
                         // distance = world units), world units on an orthographic one (nearFar 0: the test frames)
    vec4 depthWU;        // x/y: c0/c1's depth range in world units on an orthographic pass (0 on a perspective one), z: 2 tan(theta / 2)
    uvec4 mode;          // x: step 0..3 (3 = off), y: cascades, z: filter (0 = the step's, 1 = hard: one tap, cost table only)
    vec4 nearFar;        // A39: c0 near, c0 far, c1 near, c1 far - the perspective map's axis distances from 2003's point light (far 0 =
                         // an orthographic pass)
    vec4 ownPull;        // A79: 2003's light L (xyz) and eps (w; 0 = no pull) - Record/GxOwnPull.h
    vec4 ownPullAux;     // A79: x = muMax (the thin-copy class: 0 < mu <= muMax)
} gxLook;

// the blocker search's 8 taps (a Poisson disc inside the unit circle)
const vec2 kSmBlocker[8] = vec2[8](vec2(-0.613, 0.172), vec2(0.170, -0.702), vec2(0.709, 0.355), vec2(-0.153, 0.857),
                                   vec2(-0.818, -0.448), vec2(0.511, -0.276), vec2(0.022, 0.244), vec2(0.842, -0.814) * 0.85);

float smTap(uint c, vec2 uv, float dref) {
    return texture(gxSmCmp, vec3(clamp(uv, gxLook.uvClamp[c].xy, gxLook.uvClamp[c].zw), dref));
}

// 9 bilinear compare taps on a 3 x 3 grid `r` texels apart (r = 1.5: a 5 x 5 footprint)
float smPcf9(uint c, vec3 k, float r) {
    float s = 0.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            s += smTap(c, k.xy + vec2(float(i), float(j)) * r * gxLook.texel.xy, k.z);
    return s * (1.0 / 9.0);
}

// 4 bilinear compare taps half a texel off the centre: a 3 x 3 footprint
float smPcf4(uint c, vec3 k) {
    const vec2 t = gxLook.texel.xy * 0.5;
    return 0.25 * (smTap(c, k.xy + vec2(-t.x, -t.y), k.z) + smTap(c, k.xy + vec2(t.x, -t.y), k.z) +
                   smTap(c, k.xy + vec2(-t.x, t.y), k.z) + smTap(c, k.xy + vec2(t.x, t.y), k.z));
}

// A39: the map is a perspective projection from 2003's point light - a stored or receiver depth z (0..1) stands for the axis distance
// n f / (f - z (f - n)) of its cascade
float smDist(uint c, float z) {
    const vec2 nf = c == 0u ? gxLook.nearFar.xy : gxLook.nearFar.zw;
    if (nf.y <= 0.0)   // an orthographic pass: depth is linear over the cascade's range
        return z * (c == 0u ? gxLook.depthWU.x : gxLook.depthWU.y);
    return nf.x * nf.y / (nf.y - z * (nf.y - nf.x));
}

// A10 contact hardening (step 0): the blockers' mean depth inside Rmax texels, then a 9-tap PCF whose radius grows with the
// receiver-to-blocker distance in world units (Reforged's "softer with caster height"), clamped to 1..Rmax texels
float smContact(uint c, vec3 k) {
    float sum = 0.0, n = 0.0;
    for (int i = 0; i < 8; ++i) {
        const vec2 uv = clamp(k.xy + kSmBlocker[i] * gxLook.limits.w * gxLook.texel.xy, gxLook.uvClamp[c].xy, gxLook.uvClamp[c].zw);
        const float z = texture(gxSmRaw, uv).r;
        if (z < k.z) {
            sum += z;
            n += 1.0;
        }
    }
    if (n == 0.0)
        return smTap(c, k.xy, k.z);   // no blocker near: the centre tap decides (a lone thin caster is not lost)
    const float dRecv = smDist(c, k.z);
    const float distWU = dRecv - smDist(c, sum / n);
    const float farC = c == 0u ? gxLook.nearFar.y : gxLook.nearFar.w;
    const float texWU = (c == 0u ? gxLook.texel.z : gxLook.texel.w) * (farC > 0.0 ? dRecv : 1.0);   // perspective: at the receiver's distance
    const float radius = clamp(distWU * gxLook.depthWU.z / max(texWU, 1e-3), 1.0, gxLook.limits.w);
    return smPcf9(c, k, max(radius - 0.5, 0.5));
}

// One cascade: 1 = lit. `edge` = how far out in the cascade's usable square the receiver is (0 centre .. 1 its edge); `valid` = inside
// that square AND the cascade's depth range. An invalid receiver reads LIT here (A14: outside coverage is lit, never a clamped-edge
// shadow) - the caller falls back to the other cascade first (the cascades' depth ranges are fitted independently).
float smCascade(uint c, vec3 w, out float edge, out bool valid) {
    const vec4 p = gxLook.lightVP[c] * vec4(w, 1.0);
    const vec3 n = p.xyz / p.w;
    const float lim = c == 0u ? gxLook.limits.x : gxLook.limits.y;
    edge = max(abs(n.x), abs(n.y)) / lim;
    valid = edge <= 1.0 && n.z >= 0.0 && n.z <= 1.0;
    if (!valid)
        return 1.0;
    const vec3 k = vec3(n.xy * gxLook.uvMap[c].xy + gxLook.uvMap[c].zw, n.z);
    if (gxLook.mode.z == 1u)
        return smTap(c, k.xy, k.z);
    switch (gxLook.mode.x) {
        case 0u: return smContact(c, k);
        case 1u: return smPcf9(c, k, 1.5);
        default: return smPcf4(c, k);
    }
}

// c0 where it is valid (inside its square and depth range), c1 otherwise, blended over c0's outer band (A14: 10 %) only when BOTH
// are valid; step 1-2 = one cascade (c1's fit in slot 0, A12)
float smLookup(vec3 w) {
    float e0, e1;
    bool v0, v1;
    const float l0 = smCascade(0u, w, e0, v0);
    if (gxLook.mode.y < 2u || (v0 && e0 <= 1.0 - gxLook.limits.z))
        return l0;
    const float l1 = smCascade(1u, w, e1, v1);
    if (!v0)
        return l1;
    if (!v1)
        return l0;
    return mix(l0, l1, (e0 - (1.0 - gxLook.limits.z)) / gxLook.limits.z);
}

// A2/A3/A26: the forward approximation of 2003's shadow quad, at the very end (after fog): the receiver's result clamped to [0,1]
// (the UNORM target clamps before 2003's multiply), then RGB x mix(1, shadowColor, 1 - lit) where the blend scales linearly with
// dest (the translator's applyRgb bit), and alpha x mix(1, shadowColor.a, 1 - lit) only with blending AND A2C off (applyAlpha).
// The alpha test below still reads the UNdarkened alpha (2003's quad comes after the test).
vec4 gxShadowReceive(vec4 color) {
    if ((sharedData.drawBits & 1u) == 0u || gxLook.mode.x >= 3u || gxLook.mode.y == 0u)   // not a receiver / step 3 / no map
        return color;
    vec3 w = in_WorldPos;
    if ((sharedData.drawBits & 8u) != 0u) {   // A41: a clamped instance's own draw looks up where its scaled caster copy stands
        w.z = sharedData.ownZ0 + sharedData.ownScale * (w.z - sharedData.ownZ0);
        // A79 / A79a: a thin copy (0 < mu <= muMax) is replayed pulled toward L (GxShadowCasters::record) - its own receivers take the same
        // pull, so the instance compares against its copy as before; the same float decides on both sides (GxOwnPull.h)
        if (sharedData.ownScale > 0.0 && sharedData.ownScale <= gxLook.ownPullAux.x && gxLook.ownPull.w > 0.0)
            w = mix(w, gxLook.ownPull.xyz, gxLook.ownPull.w);
    }
    const float lit = smLookup(w);
    if ((sharedData.drawBits & 2u) != 0u)
        color.rgb = clamp(color.rgb, 0.0, 1.0) * mix(vec3(1.0), gxLook.shadowColor.rgb, 1.0 - lit);
    if ((sharedData.drawBits & 4u) != 0u)
        color.a *= mix(1.0, gxLook.shadowColor.a, 1.0 - lit);
    return color;
}
#endif

vec4 sample2D(uint stage, vec2 uv) {
    switch (stage) {
        case 0u: return texture(gxTex0, uv);
        case 1u: return texture(gxTex1, uv);
        case 2u: return texture(gxTex2, uv);
        case 3u: return texture(gxTex3, uv);
        case 4u: return texture(gxTex4, uv);
        case 5u: return texture(gxTex5, uv);
        case 6u: return texture(gxTex6, uv);
        default: return texture(gxTex7, uv);
    }
}

vec4 calculateFog(vec4 oColor) {
    FogState fogState = getFogState();

    vec3 fogColor = unpackUnorm4x8(sharedData.fog.packedFogColorAndAlphaRef).bgr;   // GX: was global.*
    float fogScale = sharedData.fog.fogDistanceScale;
    float fogEnd = sharedData.fog.fogDistanceEnd;
    float fogDensity = sharedData.fog.fogDensity;

    if (!fogState.enable)
        return oColor;

    float depth = fogState.useZ ? gl_FragCoord.z : (1.0 / gl_FragCoord.w);
    float fogFactor;

    switch (fogState.pixelMode) {
        case D3DFOG_NONE:
            fogFactor = in_Fog;
            break;

        // (end - d) / (end - start)
        case D3DFOG_LINEAR:
            fogFactor = fogEnd - depth;
            fogFactor = fogFactor * fogScale;
            break;

        // 1 / (e^[d * density])^2
        case D3DFOG_EXP2:
        // 1 / (e^[d * density])
        case D3DFOG_EXP:
            fogFactor = depth * fogDensity;

            if (fogState.pixelMode == D3DFOG_EXP2)
                fogFactor *= fogFactor;

            // Provides the rcp.
            fogFactor = -fogFactor;
            fogFactor = exp(fogFactor);
            break;
    }

    fogFactor = spvNClamp(fogFactor, 0.0, 1.0);

    vec4 color = oColor;
    vec3 color3 = color.rgb;
    vec3 fogFact3 = vec3(fogFactor);
    vec3 lerpedFrog = mix(fogColor, color3, fogFact3);
    return vec4(lerpedFrog.r, lerpedFrog.g, lerpedFrog.b, color.a);
}


vec4 calculateBumpmapCoords(uint stage, vec4 baseCoords, vec4 previousStageTextureVal) {
    uint previousStage = stage - 1;

    vec4 coords = baseCoords;
    vec4 m = sharedData.Stages[previousStage].BumpEnvMat;   // GX: std140 vec4 instead of float[2][2]
    [[unroll]]
    for (uint i = 0; i < 2; i++) {
        float tc_m_n = coords[i];
        vec2 bm = i == 0u ? m.xy : m.zw;
        vec2 t = previousStageTextureVal.xy;
        float result = tc_m_n + dot(bm, t);
        coords[i] = result;
    }
    return coords;
}


vec4 sampleTexture(uint stage, vec4 texcoord, vec4 previousStageTextureVal) {
    SamplerState state = getSamplerState(stage);

    if (state.projected)
        texcoord /= texcoord.w;

    uint previousStageColorOp = 0;

    if (stage > 0) {
        previousStageColorOp = getTextureStage(stage - 1u).colorOp;   // GX: DXVK 3.0 declared a shadowing local here

        if (previousStageColorOp == D3DTOP_BUMPENVMAP || previousStageColorOp == D3DTOP_BUMPENVMAPLUMINANCE)
            texcoord = calculateBumpmapCoords(stage, texcoord, previousStageTextureVal);
    }

    // GX: an unbound stage texture reads (0,0,0,1) - DXVK 2.6's unboundTextureConstId (d3d9_fixed_function.cpp:1779,1947),
    // the classic path. DXVK 3.0 returned vec4(1) here because its stage cut made a null texture reachable only through
    // PREMODULATE; GxPackFF keeps 2.6's cut, under which an untextured stage can still read TEXTURE in its alpha args.
    vec4 texVal = vec4(0.0f, 0.0f, 0.0f, 1.0f);

    if (state.type == TEXTURE_TYPE_2D)   // GX: 2D only (cube / 3D refused by GxPackFF; D3D8 FF never samples a depth texture)
        texVal = sample2D(stage, texcoord.xy);

    if (stage != 0 && previousStageColorOp == D3DTOP_BUMPENVMAPLUMINANCE) {
        float lScale = sharedData.Stages[stage - 1].BumpEnvLScale;
        float lOffset = sharedData.Stages[stage - 1].BumpEnvLOffset;
        float scale = texVal.z;
        scale *= lScale;
        scale += lOffset;
        scale = clamp(scale, 0.0, 1.0);
        texVal *= scale;
    }

    return texVal;
}


vec4 readArgValue(uint stage, uint arg, vec4 current, vec4 temp, vec4 textureVal, bool premodulate) {
    vec4 reg = vec4(1.0);
    switch (arg & D3DTA_SELECTMASK) {
        case D3DTA_CONSTANT:
            reg = decodeD3DColor(sharedData.Stages[stage].Constant);
            break;
        case D3DTA_CURRENT:
            reg = mix(current, current * textureVal, premodulate.xxxx);
            break;
        case D3DTA_DIFFUSE:
            reg = in_Color0;
            break;
        case D3DTA_SPECULAR:
            reg = in_Color1;
            break;
        case D3DTA_TEMP:
            reg = temp;
            break;
        case D3DTA_TEXTURE:
            reg = textureVal;
            break;
        case D3DTA_TFACTOR:
            reg = decodeD3DColor(sharedData.textureFactor);   // GX: was ffps.textureFactor
            break;
    }

    // reg = 1 - reg
    if ((arg & D3DTA_COMPLEMENT) != 0)
        reg = vec4(1.0) - reg;

    // reg = reg.wwww
    if ((arg & D3DTA_ALPHAREPLICATE) != 0)
        reg = reg.aaaa;

    return reg;
}

struct TextureStageArgumentValues {
    vec4 arg0;
    vec4 arg1;
    vec4 arg2;
};

TextureStageArgumentValues readArgValues(uint stage, const TextureStageArguments args, vec4 current, vec4 temp, vec4 textureVal, bool premodulate) {
    TextureStageArgumentValues argVals;
    argVals.arg0 = readArgValue(stage, args.arg0, current, temp, textureVal, premodulate);
    argVals.arg1 = readArgValue(stage, args.arg1, current, temp, textureVal, premodulate);
    argVals.arg2 = readArgValue(stage, args.arg2, current, temp, textureVal, premodulate);
    return argVals;
}

vec4 complement(vec4 val) {
    return vec4(1.0) - val;
}

vec4 saturate(vec4 val) {
    return clamp(val, vec4(0.0), vec4(1.0));
}

vec4 calculateTextureStage(uint op, vec4 dst, const TextureStageArgumentValues arg, vec4 current, vec4 textureVal) {
    switch (op) {
        case D3DTOP_SELECTARG1:
            return arg.arg1;

        case D3DTOP_SELECTARG2:
            return arg.arg2;

        case D3DTOP_MODULATE4X:
            return saturate(arg.arg1 * arg.arg2 * 4.0);

        case D3DTOP_MODULATE2X:
            return saturate(arg.arg1 * arg.arg2 * 2.0);

        case D3DTOP_MODULATE:
#ifdef GX_FF_MUTANT
            return saturate(arg.arg1 * arg.arg2 * 1.0078125);   // GX: the planted arithmetic mutant (identity gate fail arm)
#else
            return saturate(arg.arg1 * arg.arg2);
#endif

        case D3DTOP_ADDSIGNED2X:
            return saturate(2.0 * (arg.arg1 + (arg.arg2 - vec4(0.5))));

        case D3DTOP_ADDSIGNED:
            return saturate(arg.arg1 + (arg.arg2 - vec4(0.5)));

        case D3DTOP_ADD:
            return saturate(arg.arg1 + arg.arg2);

        case D3DTOP_SUBTRACT:
            return saturate(arg.arg1 - arg.arg2);

        case D3DTOP_ADDSMOOTH:
            return saturate(fma(complement(arg.arg1), arg.arg2, arg.arg1));

        case D3DTOP_BLENDDIFFUSEALPHA:
            return mix(arg.arg2, arg.arg1, in_Color0.aaaa);

        case D3DTOP_BLENDTEXTUREALPHA:
            return mix(arg.arg2, arg.arg1, textureVal.aaaa);

        case D3DTOP_BLENDFACTORALPHA:
            return mix(arg.arg2, arg.arg1, decodeD3DColor(sharedData.textureFactor).aaaa);

        case D3DTOP_BLENDTEXTUREALPHAPM:
            return saturate(fma(arg.arg2, complement(textureVal.aaaa), arg.arg1));

        case D3DTOP_BLENDCURRENTALPHA:
            return mix(arg.arg2, arg.arg1, current.aaaa);

        case D3DTOP_PREMODULATE:
            return arg.arg1;

        case D3DTOP_MODULATEALPHA_ADDCOLOR:
            return saturate(fma(arg.arg1.aaaa, arg.arg2, arg.arg1));

        case D3DTOP_MODULATECOLOR_ADDALPHA:
            return saturate(fma(arg.arg1, arg.arg2, arg.arg1.aaaa));

        case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
            return saturate(fma(complement(arg.arg1.aaaa), arg.arg2, arg.arg1));

        case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
            return saturate(fma(complement(arg.arg1), arg.arg2, arg.arg1.aaaa));

        case D3DTOP_BUMPENVMAPLUMINANCE:
        case D3DTOP_BUMPENVMAP:
            // Load texture for the next stage...
            return dst;

        case D3DTOP_DOTPRODUCT3:
            return saturate(vec4(dot(arg.arg1.rgb - vec3(0.5), arg.arg2.rgb - vec3(0.5)) * 4.0));

        case D3DTOP_MULTIPLYADD:
            return saturate(fma(arg.arg1, arg.arg2, arg.arg0));

        case D3DTOP_LERP:
            return mix(arg.arg2, arg.arg1, arg.arg0);

        default:
            // Unhandled texture op!
            return dst;

    }

    return vec4(0.0);
}


bool alphaTest(float alpha) {
    AlphaTestState alphaTest = getAlphaTestState();

    uint alphaRefInitial = bitfieldExtract(sharedData.fog.packedFogColorAndAlphaRef, 24, 8);   // GX: was global.*
    float alphaRef;

    if (alphaTest.compareOp < VK_COMPARE_OP_ALWAYS) {
        // Check if the given bit precision is supported
        bool useIntPrecision = alphaTest.precisionBits <= 8;

        if (useIntPrecision) {
            // Adjust alpha ref to the given range
            uint alphaRefInt = (alphaRefInitial << alphaTest.precisionBits) | (alphaRefInitial >> (8u - alphaTest.precisionBits));

            // Convert alpha ref to float since we'll do the comparison based on that
            alphaRef = float(alphaRefInt);

            // Adjust alpha to the given range and round
            float alphaFactor = float((256u << alphaTest.precisionBits) - 1u);

            alpha = roundEven(alpha * alphaFactor);
        } else {
            alphaRef = float(alphaRefInitial) / 255.0;
        }

        switch (alphaTest.compareOp) {
            case VK_COMPARE_OP_LESS:                return alpha <  alphaRef;
            case VK_COMPARE_OP_EQUAL:               return alpha == alphaRef;
            case VK_COMPARE_OP_LESS_OR_EQUAL:       return alpha <= alphaRef;
            case VK_COMPARE_OP_GREATER:             return alpha >  alphaRef;
            case VK_COMPARE_OP_NOT_EQUAL:           return alpha != alphaRef;
            case VK_COMPARE_OP_GREATER_OR_EQUAL:    return alpha >= alphaRef;
            default:                                return false;  // NEVER
        }
    }

    return true;
}

#ifdef GX_LOOK_A2C
// GX (B · a2c-1 20/10/2026, A108 / render-dlss SCOPE_a2c.md): alpha-to-coverage, the key-on family only (regen.sh @A2C / @SMA2C). The
// translator sets bit 3 of alphaTestAndModeArgs only on a draw whose pipeline has alphaToCoverageEnable (GxAlphaToCoverage.h: an ORDERED
// alpha test with a live ref, blending off, a multisampled target). Instead of the hard discard the alpha becomes a coverage ramp about
// one pixel wide, (a - edge) / fwidth(a) + 0.5 clamped (B. Golus, "Anti-aliased Alpha Test", 2017), with `edge` where alphaTest's
// roundEven(alpha * 255) op ref flips: a magnified edge stays where the hard test put it, and a minified one ramps over ~1 px instead
// of fading with the mip-averaged alpha (plain A2C's distant thinning). Bit 3 clear = alphaTest below, today's pixels.
bool isAlphaToCoverage() {
    return bitfieldExtract(alphaTestAndModeArgs, 3, 1) != 0;
}

float alphaCoverage(float alpha) {
    AlphaTestState alphaTest = getAlphaTestState();

    uint alphaRefInitial = bitfieldExtract(sharedData.fog.packedFogColorAndAlphaRef, 24, 8);
    uint alphaRefInt = (alphaRefInitial << alphaTest.precisionBits) | (alphaRefInitial >> (8u - alphaTest.precisionBits));
    float alphaFactor = float((256u << alphaTest.precisionBits) - 1u);
    float a = alpha * alphaFactor;

    // GE / G pass above the edge, LE / L below it; roundEven(a) flips at ref + 0.5 for G and LE, at ref - 0.5 for GE and L
    bool above = alphaTest.compareOp == VK_COMPARE_OP_GREATER || alphaTest.compareOp == VK_COMPARE_OP_GREATER_OR_EQUAL;
    bool upperEdge = alphaTest.compareOp == VK_COMPARE_OP_GREATER || alphaTest.compareOp == VK_COMPARE_OP_LESS_OR_EQUAL;
    float edge = float(alphaRefInt) + (upperEdge ? 0.5 : -0.5);
    float width = max(fwidth(a), 1.0e-4 * alphaFactor);   // uniform control flow: the branch above it reads a push / spec constant
    // B · a2c-2 (SCOPE_a2c.md "keep a discard only for alpha that is fully transparent"): the endpoint the hard
    // test can never pass - alpha 0 for GE / G, full alpha for LE / L - covers nothing, however steep the ramp next to it (a 0 | 255
    // step inside one quad gives fwidth 255: at GE 0x60 the ramp alone would cover 12.5 % of a fully transparent texel). After the
    // derivative, so fwidth stays in uniform control flow.
    if (above ? a <= 0.0 : a >= alphaFactor)
        return 0.0;
    return clamp((above ? a - edge : edge - a) / width + 0.5, 0.0, 1.0);
}
#endif

struct TextureStageState {
    vec4 current;
    vec4 temp;
    vec4 previousStageTextureVal;
    bool premodulateColor;
    bool premodulateAlpha;
};

vec4 getTexCoord(uint stage) {
    // If point sprites are enabled, we need to replace the
    // input texture coordinate with the point coordinate
    if (isPointSpriteEnabled())
        return vec4(gl_PointCoord, 0.0f, 0.0f);

    switch (stage) {
        case 0: return in_Texcoord0;
        case 1: return in_Texcoord1;
        case 2: return in_Texcoord2;
        case 3: return in_Texcoord3;
        case 4: return in_Texcoord4;
        case 5: return in_Texcoord5;
        case 6: return in_Texcoord6;
        case 7: return in_Texcoord7;
    }

    return vec4(0.0f);
}

TextureStageState runTextureStage(uint stage, TextureStageState state) {
    TextureStage info = getTextureStage(stage);

    // This cancels all subsequent stages.
    if (info.colorOp == D3DTOP_DISABLE)
        return state;

    vec4 prev = info.storeToTemp ? state.temp : state.current;
    vec4 next = prev;

    vec4 textureVal = sampleTexture(stage, getTexCoord(stage), state.previousStageTextureVal);

    // Fast path if alpha/color path is identical.
    // D3DTOP_DOTPRODUCT3 also has special quirky behaviour here.
    bool fastPath = info.colorOp == info.alphaOp &&
        info.colorArgs.arg0 == info.alphaArgs.arg0 &&
        info.colorArgs.arg1 == info.alphaArgs.arg1 &&
        info.colorArgs.arg2 == info.alphaArgs.arg2 &&
        state.premodulateColor == state.premodulateAlpha;

    if (fastPath || info.colorOp == D3DTOP_DOTPRODUCT3) {
        TextureStageArgumentValues colorArgVals = readArgValues(stage, info.colorArgs, state.current, state.temp, textureVal, state.premodulateColor);
        next = calculateTextureStage(info.colorOp, prev, colorArgVals, state.current, textureVal);
    } else {
        TextureStageArgumentValues colorArgVals = readArgValues(stage, info.colorArgs, state.current, state.temp, textureVal, state.premodulateColor);
        next.rgb = calculateTextureStage(info.colorOp, prev, colorArgVals, state.current, textureVal).rgb;

        if (info.alphaOp != D3DTOP_DISABLE) {
            TextureStageArgumentValues alphaArgVals = readArgValues(stage, info.alphaArgs, state.current, state.temp, textureVal, state.premodulateAlpha);
            next.a = calculateTextureStage(info.alphaOp, prev, alphaArgVals, state.current, textureVal).a;
        }
    }

    state.temp = info.storeToTemp ? next : state.temp;
    state.current = info.storeToTemp ? state.current : next;
    state.previousStageTextureVal = textureVal;
    state.premodulateColor = info.colorOp == D3DTOP_PREMODULATE;
    state.premodulateAlpha = info.alphaOp == D3DTOP_PREMODULATE;
    return state;
}

#ifdef GX_CUSTOM_PS
// GX (WS-E, rw2-E 11/10/2026): a custom pixel-shader port (gx_cs_*.frag) replaces the texture-stage cascade and nothing else -
// fog and the alpha test below are DXVK's own post-PS stages and run on the program's result (D3D8 applies both after a ps too).
vec4 gxCustomPs();
#endif

void main() {
#ifdef GX_CUSTOM_PS
    TextureStageState state;
    state.current = gxCustomPs();
    state.temp = vec4(0.0);
    state.previousStageTextureVal = vec4(0.0);
    state.premodulateColor = false;
    state.premodulateAlpha = false;
#else
    // in_Color0 is diffuse
    // in_Color1 is specular

    TextureStageState state;
    // Current starts of as equal to diffuse.
    state.current = in_Color0;
    // Temp starts off as equal to vec4(0)
    state.temp = vec4(0.0);
    state.previousStageTextureVal = vec4(0.0);
    state.premodulateColor = false;
    state.premodulateAlpha = false;

    // If we turn this into a loop, performance becomes very poor on the proprietary Nvidia driver
    // because it fails to unroll it.
    state = runTextureStage(0, state);
    state = runTextureStage(1, state);
    state = runTextureStage(2, state);
    state = runTextureStage(3, state);
    state = runTextureStage(4, state);
    state = runTextureStage(5, state);
    state = runTextureStage(6, state);
    state = runTextureStage(7, state);

    if (isSpecularEnabled())
        state.current.xyz += in_Color1.xyz;
#endif

    state.current = calculateFog(state.current);

#ifdef GX_LOOK_SHADOWMAP
    out_Color0 = gxShadowReceive(state.current);   // GX S7: after fog (A2); the alpha test below reads the undarkened alpha (A3)
#else
    out_Color0 = state.current;
#endif

#ifdef GX_LOOK_A2C
    if (isAlphaToCoverage()) {
        // the coverage is the alpha written (blending is off: only the target's alpha channel sees it). No sample covered = discard,
        // which is what today's test does to such a texel (no colour, no depth)
        float coverage = alphaCoverage(state.current.a);
        out_Color0.a = coverage;
        if (coverage <= 0.0)
            discard;
        return;
    }
#endif

    if (!alphaTest(state.current.a))
        discard;   // GX: was demote (GL_EXT_demote_to_helper_invocation)
}
