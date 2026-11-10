// gx_ff_common.glsl - the fixed-function ubershader's shared declarations (WS-D week 2).
//
// ADAPTED from DXVK 3.0 src/d3d9/shaders/d3d9_fixed_function_common.glsl (tag v3.0, commit 97fe0c66f; shaders/SOURCE.txt).
// DXVK is Copyright (c) 2017 Philip Rebohle, (c) 2019 Joshua Ashton, (c) 2019 Robin Kertels, (c) 2023 Jeffrey Ellison, and is
// provided under the zlib/libpng licence:
//   This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable
//   for any damages arising from the use of this software. Permission is granted to anyone to use this software for any
//   purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following
//   restrictions: 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original
//   software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but
//   is not required. 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the
//   original software. 3. This notice may not be removed or altered from any source distribution.
// ALTERED by GeneralsX (marked "GX:"): the spec-data uniform buffer is the push-constant block GxFFSpec (GxFFState.h's
// GxFFSpecBlock, 32 named words, each selected against specialisation constant i), DXVK's FF VS key words moved into it
// (vsData0-11), the shared push data became the GxFFFog struct inside the VS/PS uniform blocks, no scalar block layout.

const float FloatMaxValue = 340282346638528859811704183484516925440.0;

const uint TextureStageCount = 8;

#define D3DFOGMODE uint
const uint D3DFOG_NONE   = 0;
const uint D3DFOG_EXP    = 1;
const uint D3DFOG_EXP2   = 2;
const uint D3DFOG_LINEAR = 3;

// GX: was D3D9SharedPushData; now a member of both uniform blocks (GxFFState.h GxFFFog).
struct GxFFFog {
    uint packedFogColorAndAlphaRef;
    float fogDistanceScale;
    float fogDistanceEnd;
    float fogDensity;
};

// Thanks SPIRV-Cross
spirv_instruction(set = "GLSL.std.450", id = 79) float spvNMin(float, float);
spirv_instruction(set = "GLSL.std.450", id = 79) vec2 spvNMin(vec2, vec2);
spirv_instruction(set = "GLSL.std.450", id = 79) vec3 spvNMin(vec3, vec3);
spirv_instruction(set = "GLSL.std.450", id = 79) vec4 spvNMin(vec4, vec4);
spirv_instruction(set = "GLSL.std.450", id = 81) float spvNClamp(float, float, float);
spirv_instruction(set = "GLSL.std.450", id = 81) vec2 spvNClamp(vec2, vec2, vec2);
spirv_instruction(set = "GLSL.std.450", id = 81) vec3 spvNClamp(vec3, vec3, vec3);
spirv_instruction(set = "GLSL.std.450", id = 81) vec4 spvNClamp(vec4, vec4, vec4);

// GX: the FF key (was `layout(set = SPEC_DATA_SET, binding = 0, scalar) uniform SpecConsts`). Word i = GxFFSpecWord i.
// ONE SPIR-V module serves both pipelines: every key word is read as `gxSpecialised ? <spec constant i> : <push word i>`.
// GENERIC pipeline: no VkSpecializationInfo -> gxSpecialised is false and the words come from the push block at run time.
// SPECIALISED pipeline: gxSpecialised = true and constant i = word i (VkSpecializationInfo), so the driver folds the key.
// (DXVK 3.0 gets the same pair by rewriting its spec-data buffer into OpSpecConstants; selecting on a spec constant needs no
// SPIR-V rewriting at all.) Read every word by NAME - an array or a dynamic index would hide it from the specialisation.
layout(constant_id = 32) const bool gxSpecialised = false;
layout(constant_id = 0) const uint gxSpec_alphaTestAndModeArgs = 0u;
layout(constant_id = 1) const uint gxSpec_fogArgs = 0u;
layout(constant_id = 2) const uint gxSpec_packedProjMaskAndFfArgs = 0u;
layout(constant_id = 3) const uint gxSpec_unused_vsSamplerTypesAndBools = 0u;
layout(constant_id = 4) const uint gxSpec_psSamplerTypes = 0u;
layout(constant_id = 5) const uint gxSpec_psSamplerModes = 0u;
layout(constant_id = 6) const uint gxSpec_unused_psBools = 0u;
layout(constant_id = 7) const uint gxSpec_unused_spec7 = 0u;
layout(constant_id = 8) const uint gxSpec_packedStageOps01 = 0u;
layout(constant_id = 9) const uint gxSpec_packedStageOps23 = 0u;
layout(constant_id = 10) const uint gxSpec_packedStageOps45 = 0u;
layout(constant_id = 11) const uint gxSpec_packedStageOps67 = 0u;
layout(constant_id = 12) const uint gxSpec_packedStageArgs0 = 0u;
layout(constant_id = 13) const uint gxSpec_packedStageArgs1 = 0u;
layout(constant_id = 14) const uint gxSpec_packedStageArgs2 = 0u;
layout(constant_id = 15) const uint gxSpec_packedStageArgs3 = 0u;
layout(constant_id = 16) const uint gxSpec_packedStageArgs4 = 0u;
layout(constant_id = 17) const uint gxSpec_packedStageArgs5 = 0u;
layout(constant_id = 18) const uint gxSpec_packedStageArgs6 = 0u;
layout(constant_id = 19) const uint gxSpec_packedStageArgs7 = 0u;
layout(constant_id = 20) const uint gxSpec_vsData0 = 0u;
layout(constant_id = 21) const uint gxSpec_vsData1 = 0u;
layout(constant_id = 22) const uint gxSpec_vsData2 = 0u;
layout(constant_id = 23) const uint gxSpec_vsData3 = 0u;
layout(constant_id = 24) const uint gxSpec_vsData4 = 0u;
layout(constant_id = 25) const uint gxSpec_vsData5 = 0u;
layout(constant_id = 26) const uint gxSpec_vsData6 = 0u;
layout(constant_id = 27) const uint gxSpec_vsData7 = 0u;
layout(constant_id = 28) const uint gxSpec_vsData8 = 0u;
layout(constant_id = 29) const uint gxSpec_vsData9 = 0u;
layout(constant_id = 30) const uint gxSpec_vsData10 = 0u;
layout(constant_id = 31) const uint gxSpec_vsData11 = 0u;
layout(push_constant) uniform GxFFSpec {
    uint alphaTestAndModeArgs;
    uint fogArgs;
    uint packedProjMaskAndFfArgs;
    uint unused_vsSamplerTypesAndBools;
    uint psSamplerTypes;
    uint psSamplerModes;
    uint unused_psBools;
    uint unused_spec7;
    uint packedStageOps01;
    uint packedStageOps23;
    uint packedStageOps45;
    uint packedStageOps67;
    uint packedStageArgs0;
    uint packedStageArgs1;
    uint packedStageArgs2;
    uint packedStageArgs3;
    uint packedStageArgs4;
    uint packedStageArgs5;
    uint packedStageArgs6;
    uint packedStageArgs7;
    uint vsData0;
    uint vsData1;
    uint vsData2;
    uint vsData3;
    uint vsData4;
    uint vsData5;
    uint vsData6;
    uint vsData7;
    uint vsData8;
    uint vsData9;
    uint vsData10;
    uint vsData11;
} gxPush;
#define alphaTestAndModeArgs (gxSpecialised ? gxSpec_alphaTestAndModeArgs : gxPush.alphaTestAndModeArgs)
#define fogArgs (gxSpecialised ? gxSpec_fogArgs : gxPush.fogArgs)
#define packedProjMaskAndFfArgs (gxSpecialised ? gxSpec_packedProjMaskAndFfArgs : gxPush.packedProjMaskAndFfArgs)
#define unused_vsSamplerTypesAndBools (gxSpecialised ? gxSpec_unused_vsSamplerTypesAndBools : gxPush.unused_vsSamplerTypesAndBools)
#define psSamplerTypes (gxSpecialised ? gxSpec_psSamplerTypes : gxPush.psSamplerTypes)
#define psSamplerModes (gxSpecialised ? gxSpec_psSamplerModes : gxPush.psSamplerModes)
#define unused_psBools (gxSpecialised ? gxSpec_unused_psBools : gxPush.unused_psBools)
#define unused_spec7 (gxSpecialised ? gxSpec_unused_spec7 : gxPush.unused_spec7)
#define packedStageOps01 (gxSpecialised ? gxSpec_packedStageOps01 : gxPush.packedStageOps01)
#define packedStageOps23 (gxSpecialised ? gxSpec_packedStageOps23 : gxPush.packedStageOps23)
#define packedStageOps45 (gxSpecialised ? gxSpec_packedStageOps45 : gxPush.packedStageOps45)
#define packedStageOps67 (gxSpecialised ? gxSpec_packedStageOps67 : gxPush.packedStageOps67)
#define packedStageArgs0 (gxSpecialised ? gxSpec_packedStageArgs0 : gxPush.packedStageArgs0)
#define packedStageArgs1 (gxSpecialised ? gxSpec_packedStageArgs1 : gxPush.packedStageArgs1)
#define packedStageArgs2 (gxSpecialised ? gxSpec_packedStageArgs2 : gxPush.packedStageArgs2)
#define packedStageArgs3 (gxSpecialised ? gxSpec_packedStageArgs3 : gxPush.packedStageArgs3)
#define packedStageArgs4 (gxSpecialised ? gxSpec_packedStageArgs4 : gxPush.packedStageArgs4)
#define packedStageArgs5 (gxSpecialised ? gxSpec_packedStageArgs5 : gxPush.packedStageArgs5)
#define packedStageArgs6 (gxSpecialised ? gxSpec_packedStageArgs6 : gxPush.packedStageArgs6)
#define packedStageArgs7 (gxSpecialised ? gxSpec_packedStageArgs7 : gxPush.packedStageArgs7)
#define vsData0 (gxSpecialised ? gxSpec_vsData0 : gxPush.vsData0)
#define vsData1 (gxSpecialised ? gxSpec_vsData1 : gxPush.vsData1)
#define vsData2 (gxSpecialised ? gxSpec_vsData2 : gxPush.vsData2)
#define vsData3 (gxSpecialised ? gxSpec_vsData3 : gxPush.vsData3)
#define vsData4 (gxSpecialised ? gxSpec_vsData4 : gxPush.vsData4)
#define vsData5 (gxSpecialised ? gxSpec_vsData5 : gxPush.vsData5)
#define vsData6 (gxSpecialised ? gxSpec_vsData6 : gxPush.vsData6)
#define vsData7 (gxSpecialised ? gxSpec_vsData7 : gxPush.vsData7)
#define vsData8 (gxSpecialised ? gxSpec_vsData8 : gxPush.vsData8)
#define vsData9 (gxSpecialised ? gxSpec_vsData9 : gxPush.vsData9)
#define vsData10 (gxSpecialised ? gxSpec_vsData10 : gxPush.vsData10)
#define vsData11 (gxSpecialised ? gxSpec_vsData11 : gxPush.vsData11)

// Fog parameters
struct FogState {
    bool enable;
    uint pixelMode;
    uint vertexMode;
    bool useZ;
};

FogState getFogState() {
    FogState result;
    result.enable = bitfieldExtract(fogArgs, 0, 1) != 0u;
    result.vertexMode = bitfieldExtract(fogArgs, 8, 2);
    result.pixelMode = bitfieldExtract(fogArgs, 16, 2);
    result.useZ = bitfieldExtract(fogArgs, 24, 1) != 0u;
    return result;
}
// Checks whether specular lighting is enabled
bool isSpecularEnabled() {
    return bitfieldExtract(packedProjMaskAndFfArgs, 8, 1) != 0;
}

vec4 decodeD3DColor(uint color) {
    return unpackUnorm4x8(color).bgra;
}
