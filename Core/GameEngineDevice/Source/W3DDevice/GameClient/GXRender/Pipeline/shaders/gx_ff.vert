#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_spirv_intrinsics : require

// gx_ff.vert - the fixed-function vertex ubershader (WS-D week 2, C · rw2-D, 11/10/2026).
// ADAPTED from DXVK 3.0 src/d3d9/shaders/d3d9_fixed_function_vert.vert (tag v3.0, commit 97fe0c66f; zlib/libpng licence,
// notice in gx_ff_common.glsl and shaders/SOURCE.txt). ALTERED by GeneralsX (marked "GX:"): set 0 binding 0 std140 uniform
// block GxFFVs (GxFFState.h GxFFVsBlock); the FF VS key read from the push block (vsData0-11) instead of the uniform buffer;
// point size / scale and fog parameters read from the block instead of push data; vertex blend, tween and user clip planes
// removed (GxPackFF refuses them: the census never met them and the game never enables them).

invariant gl_Position;

// The locations need to match with RegisterLinkerSlot in dxso_util.cpp (= GxSemantic, the generic set's vertex inputs)
layout(location = 0) in vec4 in_Position0;
layout(location = 1) in vec4 in_Normal0;
layout(location = 2) in vec4 in_Position1;
layout(location = 3) in vec4 in_Normal1;
layout(location = 4) in vec4 in_Texcoord0;
layout(location = 5) in vec4 in_Texcoord1;
layout(location = 6) in vec4 in_Texcoord2;
layout(location = 7) in vec4 in_Texcoord3;
layout(location = 8) in vec4 in_Texcoord4;
layout(location = 9) in vec4 in_Texcoord5;
layout(location = 10) in vec4 in_Texcoord6;
layout(location = 11) in vec4 in_Texcoord7;
layout(location = 12) in vec4 in_Color0;
layout(location = 13) in vec4 in_Color1;
layout(location = 14) in float in_Fog;
layout(location = 15) in float in_PointSize;
layout(location = 16) in vec4 in_BlendWeight;
layout(location = 17) in uvec4 in_BlendIndices;   // GX: uvec4 = the generic set's _UINT null-stream format (blend refused, unread)

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
// GX S7 (D · look-shadowmaps-2, PREREG_look-shadowmaps A20/A36, PLAN P2): the key-on family (regen.sh "@SM") writes the WORLD position the
// receiver's shadow-map lookup needs (the key-on fragment shader reads location 12; every vertex shader it pairs with writes it).
layout(location = 12) out vec3 out_WorldPos;
#endif

#include "gx_ff_common.glsl"

const uint MaxEnabledLights = 8;

struct D3D9ViewportInfo {
    vec4 inverseOffset;
    vec4 inverseExtent;
};

#define D3DLIGHTTYPE uint
const uint D3DLIGHT_POINT       = 1;
const uint D3DLIGHT_SPOT        = 2;
const uint D3DLIGHT_DIRECTIONAL = 3;

struct D3D9Light {
    vec4 Diffuse;
    vec4 Specular;
    vec4 Ambient;

    vec4 Position;
    vec4 Direction;

    D3DLIGHTTYPE Type;
    float Range;
    float Falloff;
    float Attenuation0;
    float Attenuation1;
    float Attenuation2;
    float Theta;
    float Phi;
};

#define D3DCOLORVALUE vec4

struct D3DMATERIAL9 {
    D3DCOLORVALUE   Diffuse;
    D3DCOLORVALUE   Ambient;
    D3DCOLORVALUE   Specular;
    D3DCOLORVALUE   Emissive;
    float           Power;
};

// GX: = GxFFState.h GxFFVsBlock (std140; the key bytes moved to the push block, fog and point parameters moved in).
struct D3D9FixedFunctionVS {
    mat4 WorldView;
    mat4 NormalMatrix;
    mat4 InverseView;
    mat4 Projection;
    mat4 WorldViewProj;

    mat4 TexcoordMatrices[TextureStageCount];

    D3D9ViewportInfo ViewportInfo;

    D3D9Light Lights[MaxEnabledLights];
    D3DMATERIAL9 Material;
    GxFFFog Fog;
    uint GlobalAmbient;
    float TweenFactor;
    float PointSize;
    float PointSizeMin;
    float PointSizeMax;
    float PointScaleA;
    float PointScaleB;
    float PointScaleC;
};

#define D3DMATERIALCOLORSOURCE uint
const uint D3DMCS_MATERIAL = 0;
const uint D3DMCS_COLOR1   = 1;
const uint D3DMCS_COLOR2   = 2;

#define D3DTEXTURETRANSFORMFLAGS uint
const uint D3DTTFF_DISABLE   = 0;
const uint D3DTTFF_COUNT1    = 1;
const uint D3DTTFF_COUNT2    = 2;
const uint D3DTTFF_COUNT3    = 3;
const uint D3DTTFF_COUNT4    = 4;
const uint D3DTTFF_PROJECTED = 256;

const uint DXVK_TSS_TCI_PASSTHRU                    = 0x00000000;
const uint DXVK_TSS_TCI_CAMERASPACENORMAL           = 0x00010000;
const uint DXVK_TSS_TCI_CAMERASPACEPOSITION         = 0x00020000;
const uint DXVK_TSS_TCI_CAMERASPACEREFLECTIONVECTOR = 0x00030000;
const uint DXVK_TSS_TCI_SPHEREMAP                   = 0x00040000;

const uint TCIOffset = 16;
const uint TCIMask = (7 << TCIOffset);

// Checks whether point scaling is
bool isPointScalingEnabled() {
    return bitfieldExtract(packedProjMaskAndFfArgs, 16, 1) != 0;
}

// Checks whether active pixel shader is SM3
bool boundPsIsShaderModel3() {
    return bitfieldExtract(packedProjMaskAndFfArgs, 24, 1) != 0;
}

// Checks sampler projection state without accessing all sampler state
bool isSamplerProjected(uint idx) {
    return bitfieldExtract(packedProjMaskAndFfArgs, int(idx), 1) != 0;
}

layout(set = 0, binding = 0, std140, row_major)   // GX: was set = CBV_SET, binding = CBV_VS_FIXED_FUNCTION, scalar
uniform ShaderData {
    D3D9FixedFunctionVS data;
};

// GX: the point size, min and max are floats in the block (DXVK packed them as 13.3 fixed point in push data).
vec3 decodePointSize() {
    return vec3(data.PointSize, data.PointSizeMin, data.PointSizeMax);
}

// Functions to extract information from the VS key.
// GX: the key lives in the push block's vsData0-11 (DXVK: data.DataPrimitives[12] in the uniform buffer). Each word is read
// by name so the specialised pipeline sees constants.
uint vsKeyWord(uint index) {
    switch (index) {
        case 0u:  return vsData0;
        case 1u:  return vsData1;
        case 2u:  return vsData2;
        case 3u:  return vsData3;
        case 4u:  return vsData4;
        case 5u:  return vsData5;
        case 6u:  return vsData6;
        case 7u:  return vsData7;
        case 8u:  return vsData8;
        case 9u:  return vsData9;
        case 10u: return vsData10;
        default:  return vsData11;
    }
}

uint extractByte(uint offset) {
    uint dword = vsKeyWord(offset / 4u);
    return bitfieldExtract(dword, (int(offset) % 4) * 8, 8);
}

uint texcoordIndex(uint index) {
    return extractByte(index);
}
bool vertexHasPositionT() {
    return extractByte(28) != 0;
}
bool vertexHasColor0() {
    return extractByte(29) != 0;
}
bool vertexHasColor1() {
    return extractByte(30) != 0;
}
bool vertexHasPointSize() {
    return extractByte(31) != 0;
}
bool useLighting() {
    return extractByte(40) != 0;
}
bool normalizeNormals() {
    return extractByte(37) != 0;
}
bool localViewer() {
    return extractByte(38) != 0;
}
bool rangeFog() {
    return extractByte(39) != 0;
}

uint texcoordFlags(uint index) {
    return extractByte(TextureStageCount + index);
}
uint diffuseSource() {
    return extractByte(42);
}
uint ambientSource() {
    return extractByte(43);
}
uint specularSource() {
    return extractByte(44);
}
uint emissiveSource() {
    return extractByte(45);
}

uint texcoordTransformFlags(uint index) {
    return extractByte(TextureStageCount * 2 + index);
}
uint lightCount() {
    return extractByte(41);
}

uint vertexTexcoordDeclMask() {
    return vsData6;   // GX: DataPrimitives[24 / 4]
}
bool vertexHasFog() {
    return extractByte(32) != 0;
}


float calculateFog(vec4 vPos) {
    FogState fogState = getFogState();

    vec4 specular = in_Color1;
    bool hasSpecular = vertexHasColor1();

    float fogScale = data.Fog.fogDistanceScale;   // GX: was global.* (push data)
    float fogEnd = data.Fog.fogDistanceEnd;
    float fogDensity = data.Fog.fogDensity;

    if (!fogState.enable)
        return 0.0;

    float w = vPos.w;
    float z = vPos.z;
    float depth;
    if (rangeFog()) {
        vec3 pos3 = vPos.xyz;
        depth = length(pos3);
    } else {
        depth = vertexHasFog() ? in_Fog : abs(z);
    }
    float fogFactor;
    if (vertexHasPositionT()) {
        fogFactor = hasSpecular ? specular.w : 1.0;
    } else {
        switch (fogState.vertexMode) {
            default:
            case D3DFOG_NONE:
                fogFactor = hasSpecular ? specular.w : 1.0;
                break;

            // (end - d) / (end - start)
            case D3DFOG_LINEAR:
                fogFactor = fogEnd - depth;
                fogFactor = fogFactor * fogScale;
                fogFactor = spvNClamp(fogFactor, 0.0, 1.0);
                break;

            // 1 / (e^[d * density])^2
            case D3DFOG_EXP2:
            // 1 / (e^[d * density])
            case D3DFOG_EXP:
                fogFactor = depth * fogDensity;

                if (fogState.vertexMode == D3DFOG_EXP2)
                    fogFactor *= fogFactor;

                // Provides the rcp.
                fogFactor = -fogFactor;
                fogFactor = exp(fogFactor);
                break;
        }
    }

    return fogFactor;
}


float calculatePointSize(vec4 vtx) {
    vec3 pointSizeArgs = decodePointSize();

    float value = vertexHasPointSize() ? in_PointSize : pointSizeArgs.x;

    float scaleC = data.PointScaleC;   // GX: was ffvs.* (push data)
    float scaleB = data.PointScaleB;
    float scaleA = data.PointScaleA;

    vec3 vtx3 = vtx.xyz;

    float DeSqr = dot(vtx3, vtx3);
    float De    = sqrt(DeSqr);
    float scaleValue = scaleC * DeSqr;
    scaleValue = fma(scaleB, De, scaleValue);
    scaleValue += scaleA;
    scaleValue = sqrt(scaleValue);
    scaleValue = value / scaleValue;

    value = isPointScalingEnabled() ? scaleValue : value;

    float pointSizeMin = pointSizeArgs.y;
    float pointSizeMax = pointSizeArgs.z;

    return clamp(value, pointSizeMin, pointSizeMax);
}

// Precise dot product and matrix-vector product helpers. This needs to match
// programmable shaders since some games rely on fixed-function and programmable
// VS producing identical vertex positions (e.g. Railroad Tycoon 3).
precise float dp4(vec4 a, vec4 b) {
    return fma(a.w, b.w, fma(a.z, b.z, fma(a.y, b.y, a.x * b.x)));
}

precise vec4 vecTimesMat4(vec4 a, mat4 b) {
    return vec4(dp4(a, b[0]), dp4(a, b[1]),
                dp4(a, b[2]), dp4(a, b[3]));
}

float mul_legacy(float a, float b) {
    return (b == 0.0f ? 0.0f : a) * (a == 0.0f ? 0.0f : b);
}


vec4 pickMaterialSource(uint source, vec4 material) {
    if (source == D3DMCS_COLOR1 && vertexHasColor0())
        return in_Color0;
    else if (source == D3DMCS_COLOR2 && vertexHasColor1())
        return in_Color1;
    else
        return material;
}


struct Vertex {
    vec4 coord;
    vec4 transformed;
    vec3 normal;
};


Vertex transformVertex() {
    Vertex result;
    result.coord = in_Position0;
    result.normal = in_Normal0.xyz;

    // GX: tween and vertex blend removed (GxPackFF refuses D3DRS_VERTEXBLEND != 0).

    if (!vertexHasPositionT()) {
        // GX: two steps, (v x WorldView) x Projection, exactly as DXVK 2.6 - the classic path players get today
        // (references/fbraz3-dxvk d3d9_fixed_function.cpp:1049/1077, emitVectorTimesMatrix = the same fma order as dp4).
        // DXVK 3.0 used the CPU-premultiplied WorldViewProj (for Railroad Tycoon 3), which rounds differently and moved MSAA
        // edge coverage: E · rw2-F1-2 measured the G2 capV f600 drawable subset at 99.8555% -> 99.9974% identical to the
        // classic replay with this change alone (render-seam/rw2-F1/farm/draw-wvpx/).
        result.coord = vecTimesMat4(result.coord, data.WorldView);
        result.transformed = vecTimesMat4(result.coord, data.Projection);
        result.normal = mat3(data.NormalMatrix) * result.normal;

        if (normalizeNormals()) {
            float normalScale = inversesqrt(dot(result.normal, result.normal));
            result.normal.x = mul_legacy(result.normal.x, normalScale);
            result.normal.y = mul_legacy(result.normal.y, normalScale);
            result.normal.z = mul_legacy(result.normal.z, normalScale);
        }

        return result;
    } else {
        // We still need to account for perspective correction here...
        result.transformed = fma(result.coord, data.ViewportInfo.inverseExtent, data.ViewportInfo.inverseOffset);

        float rhw = result.transformed.w == 0.0 ? 1.0 : 1.0 / result.transformed.w;
        result.transformed.xyz *= rhw;
        result.transformed.w = rhw;
        return result;
    }
}


vec4 loadTexcoord(uint idx) {
    vec4 result = vec4(0.0f);
    result = mix(result, in_Texcoord0, bvec4(idx == 0u));
    result = mix(result, in_Texcoord1, bvec4(idx == 1u));
    result = mix(result, in_Texcoord2, bvec4(idx == 2u));
    result = mix(result, in_Texcoord3, bvec4(idx == 3u));
    result = mix(result, in_Texcoord4, bvec4(idx == 4u));
    result = mix(result, in_Texcoord5, bvec4(idx == 5u));
    result = mix(result, in_Texcoord6, bvec4(idx == 6u));
    result = mix(result, in_Texcoord7, bvec4(idx == 7u));
    return result;
}


float vectorExtract(vec4 vector, uint idx) {
    float result = vector.x;
    result = mix(result, vector.y, idx == 1u);
    result = mix(result, vector.z, idx == 2u);
    result = mix(result, vector.w, idx == 3u);
    return result;
}


vec4 transformTexCoord(uint idx, vec4 vertex, vec3 normal) {
    // 0b111 = 7
    uint inputIndex = texcoordIndex(idx);
    uint inputFlags = texcoordFlags(idx) << TCIOffset;
    uint texcoordCount = bitfieldExtract(vertexTexcoordDeclMask(), int(inputIndex) * 3, 3);

    vec4 transformed = vec4(0.0f);

    uint flags = texcoordTransformFlags(idx);

    // Passing 0xffffffff results in it getting clamped to the dimensions of the texture coords and getting treated as PROJECTED
    // but D3D9 does not apply the transformation matrix.
    bool applyTransform = flags > D3DTTFF_COUNT1 && flags <= D3DTTFF_COUNT4;

    uint count = min(flags, 4u);

    // A projection component index of 4 means we won't do projection
    uint projIndex = count != 0u ? count - 1u : 4u;

    switch (inputFlags) {
        default:
        case DXVK_TSS_TCI_PASSTHRU:
            transformed = loadTexcoord(inputIndex & 0xffu);

            if (texcoordCount < 4u) {
                // Vulkan sets the w component to 1.0 if that's not provided by the vertex buffer, D3D9 expects 0 here
                transformed.w = 0.0;
            }

            if (applyTransform && !vertexHasPositionT()) {
                /*This doesn't happen every time and I cannot figure out the difference between when it does and doesn't.
                Keep it disabled for now, it's more likely that games rely on the zero texcoord than the weird 1 here.
                if (texcoordCount <= 1) {
                    // y gets padded to 1 for some reason
                    transformed.y = 1.0;
                }*/

                // The first component after the last one thats backed by a vertex buffer gets padded to 1 for some reason.
                if (texcoordCount >= 1u)
                    transformed = mix(transformed, vec4(1.0f), equal(uvec4(0u, 1u, 2u, 3u), texcoordCount.xxxx));
            } else if (texcoordCount != 0 && !applyTransform) {
                // COUNT0, COUNT1, COUNT > 4 => take count from vertex decl if that's not zero
                count = texcoordCount;
            }

            projIndex = count != 0u ? count - 1u : 4u;
            break;

        case DXVK_TSS_TCI_CAMERASPACENORMAL:
            transformed = vec4(normal, 1.0f);

            if (!applyTransform) {
                count = 3u;
                projIndex = 4u;
            }
            break;

        case DXVK_TSS_TCI_CAMERASPACEPOSITION:
            transformed = vertex;
            if (!applyTransform) {
                count = 3u;
                projIndex = 4u;
            }
            break;

        case DXVK_TSS_TCI_CAMERASPACEREFLECTIONVECTOR: {
            vec3 reflection = reflect(normalize(vertex.xyz), normal);
            transformed = vec4(reflection, 1.0f);

            if (!applyTransform) {
                count = 3u;
                projIndex = 4u;
            }

            break;
        }

        case DXVK_TSS_TCI_SPHEREMAP: {
            vec3 reflection = reflect(normalize(vertex.xyz), normal);

            float m = length(reflection + vec3(0.0f, 0.0f, 1.0f)) * 2.0f;
            transformed = vec4(reflection.xy / m + 0.5f, 0.0f, 1.0f);
            break;
        }
    }

    if (applyTransform && !vertexHasPositionT())
        transformed = transformed * data.TexcoordMatrices[idx];

    // Discard the components that exceed the specified D3DTTFF_COUNT
    vec4 result = mix(transformed, vec4(0.0f), lessThan(count.xxxx, uvec4(1u, 2u, 3u, 4u)));

    if (isSamplerProjected(idx) && projIndex < 4u) {
        // The projection idx is always based on the flags, even when the input
        // mode is not DXVK_TSS_TCI_PASSTHRU. The w component is only used for
        // projection or unused, so always insert the divisor there. The pixel
        // shader will then decide whether to project or not.
        result.w = vectorExtract(transformed, projIndex);
    }

    return result;
}


struct Lighting {
    vec4 diffuse;
    vec4 specular;
};


Lighting computeLighting(vec4 vertex, vec3 normal) {
    Lighting result;
    result.diffuse = vertexHasColor0() ? in_Color0 : vec4(1.0, 1.0, 1.0, 1.0);
    result.specular = vertexHasColor1() ? in_Color1 : vec4(0.0, 0.0, 0.0, boundPsIsShaderModel3() ? 0.0 : 1.0);

    if (useLighting()) {
        vec4 diffuseValue = vec4(0.0);
        vec4 specularValue = vec4(0.0);
        vec4 ambientValue = vec4(0.0);

        for (uint i = 0; i < lightCount(); i++) {
            D3D9Light light = data.Lights[i];

            vec3 delta = light.Position.xyz - vertex.xyz;
            float dist = length(delta);

            // Directional light properties
            // GX: normalized for every light type, as DXVK 2.6 (d3d9_fixed_function.cpp:1301 opNormalize after the select; 3.0
            // skips it for directional lights) - GxPackFF's direction is unit length already, but the re-normalize rounds
            vec3 hitDir = normalize(-light.Direction.xyz);
            float atten = 1.0f;

            if (light.Type != D3DLIGHT_DIRECTIONAL) {
                // Range-based attenuation
                atten = fma(dist, light.Attenuation2, light.Attenuation1);
                atten = fma(dist, atten, light.Attenuation0);
                atten = 1.0 / atten;
                atten = spvNMin(atten, FloatMaxValue);
                atten = dist > light.Range ? 0.0 : atten;

                hitDir = normalize(delta);
            }

            if (light.Type == D3DLIGHT_SPOT) {
                // Angle-based attenuation
                float rho = dot(-hitDir, light.Direction.xyz);
                float spotAtten = rho - light.Phi;
                      spotAtten = spotAtten / (light.Theta - light.Phi);
                      spotAtten = pow(spotAtten, light.Falloff);

                bool insideThetaAndPhi = rho <= light.Theta;
                bool insidePhi = rho > light.Phi;
                     spotAtten = insidePhi ? spotAtten : 0.0;
                     spotAtten = insideThetaAndPhi ? spotAtten : 1.0;
                     spotAtten = clamp(spotAtten, 0.0, 1.0);

                atten *= spotAtten;
            }

            // Ambient + Diffuse
            float hitDot = dot(normal, hitDir);
                  hitDot = clamp(hitDot, 0.0, 1.0);

            float diffuseness = hitDot * atten;
            ambientValue += light.Ambient * atten;
            diffuseValue += light.Diffuse * diffuseness;

            // Specular
            vec3 mid;

            if (localViewer()) {
                mid = normalize(vertex.xyz);
                mid = hitDir - mid;
            } else {
                mid = hitDir - vec3(0.0, 0.0, 1.0);
            }

            float midDot = dot(normal, normalize(mid));
                  midDot = clamp(midDot, 0.0, 1.0);

            if (midDot > 0.0 && hitDot > 0.0) {
                float specularness = pow(midDot, data.Material.Power) * atten;
                specularValue += light.Specular * specularness;
            }
        }

        vec4 matDiffuse  = pickMaterialSource(diffuseSource(), data.Material.Diffuse);
        vec4 matAmbient  = pickMaterialSource(ambientSource(), data.Material.Ambient);
        vec4 matEmissive = pickMaterialSource(emissiveSource(), data.Material.Emissive);
        vec4 matSpecular = pickMaterialSource(specularSource(), data.Material.Specular);

        vec4 finalColor0 = fma(matAmbient, decodeD3DColor(data.GlobalAmbient), matEmissive);
             finalColor0 = fma(matAmbient, ambientValue, finalColor0);
             finalColor0 = fma(matDiffuse, diffuseValue, finalColor0);
             finalColor0.a = matDiffuse.a;

        vec4 finalColor1 = matSpecular * specularValue;

        // Saturate
        finalColor0 = clamp(finalColor0, vec4(0.0), vec4(1.0));
        finalColor1 = clamp(finalColor1, vec4(0.0), vec4(1.0));

        result.diffuse = finalColor0;

        if (isSpecularEnabled())
            result.specular = finalColor1;
    }

    return result;
}


void main() {
    Vertex vtx = transformVertex();

    gl_Position = vtx.transformed;
    gl_PointSize = calculatePointSize(vtx.coord);

    // GX: emitVsClipping removed (user clip planes are refused).

    out_Normal = vec4(vtx.normal, 1.0);

    out_Texcoord0 = transformTexCoord(0u, vtx.coord, vtx.normal);
    out_Texcoord1 = transformTexCoord(1u, vtx.coord, vtx.normal);
    out_Texcoord2 = transformTexCoord(2u, vtx.coord, vtx.normal);
    out_Texcoord3 = transformTexCoord(3u, vtx.coord, vtx.normal);
    out_Texcoord4 = transformTexCoord(4u, vtx.coord, vtx.normal);
    out_Texcoord5 = transformTexCoord(5u, vtx.coord, vtx.normal);
    out_Texcoord6 = transformTexCoord(6u, vtx.coord, vtx.normal);
    out_Texcoord7 = transformTexCoord(7u, vtx.coord, vtx.normal);

    Lighting lighting = computeLighting(vtx.coord, vtx.normal);
    out_Color0 = lighting.diffuse;
    out_Color1 = lighting.specular;

    out_Fog = calculateFog(vtx.coord);
#ifdef GX_LOOK_SHADOWMAP
    // vtx.coord is the view-space position; InverseView holds transpose(inverse(View)) read row_major, so M * v = v * View^-1 (the
    // D3D row-vector product) = the world position. A pre-transformed vertex has none: it never receives (the translator's rule).
    out_WorldPos = vertexHasPositionT() ? vec3(0.0) : (data.InverseView * vtx.coord).xyz;
#endif
}
