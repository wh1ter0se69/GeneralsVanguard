#version 450
// gx_cs_vel.vert - the object-velocity vertex program of the DLSS-track motion bridge (D · render-motion-units-1, 13/10/2026;
// modernize/render-dlss/DESIGN_motion-units.md §2.7). Synthetic catalogue programs gx:vel-rigid (this file) and gx:vel-skin
// (-DGX_VEL_SKIN): the game re-draws a moved rigid or skinned draw into the object-velocity image O at world End, depth-tested
// LESSEQUAL against the world depth, and gx_cs_vel.frag (gx:vel-ps) writes (prevPx - curPx, 0, 1).
//
// Constants (GxSyntheticPrograms.h): c0-3 current world*view (skin: view) = the bytes GxPackFFWorld computes, c4-7 current
// projection as the device draws it (JITTERED, GxWorldPassJitterProjection), c8-11 previous world*view*projection (skin:
// view*projection), c12 = (0.5 viewport W, -0.5 viewport H, zEps, 0), c13 = (jitter x, jitter y in NDC, 0, 0). Matrices are row-vector
// d3d matrices uploaded transposed: c[base + i] = column i, the vector gx_ff.vert's vecTimesMat4 dots with. Position = the FF vertex
// shader's own precise two-step arithmetic on the same inputs, so the depth (and, but for jitter rounding, x/y) is the FF draw's:
// the replay passes LESSEQUAL against the world depth with zEps 0.
//
// Interface = the FF vertex shader's, like gx_cs_trees.vert, so the custom fragment module links unchanged. Texcoord6 / 7 carry
// the previous / current (unjittered) clip position pre-scaled to pixels (xy * c12.xy, w); the fragment divides per pixel.
invariant gl_Position;

layout(location = 0) in vec4 in_v0;    // GxSemPosition (decl FLOAT3): rigid object-space / skin current world-space position
#ifdef GX_VEL_SKIN
layout(location = 1) in vec4 in_v1;    // GxSemNormal (decl FLOAT3): skin previous world-space position
#endif

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

layout(set = 0, binding = 0, std140) uniform GxCsVs { vec4 c[96]; };

// gx_ff.vert:331-340, verbatim: the FF vertex shader's precise dot product and matrix-vector product
precise float dp4(vec4 a, vec4 b) {
    return fma(a.w, b.w, fma(a.z, b.z, fma(a.y, b.y, a.x * b.x)));
}

precise vec4 vecTimesMat4(vec4 a, mat4 b) {
    return vec4(dp4(a, b[0]), dp4(a, b[1]),
                dp4(a, b[2]), dp4(a, b[3]));
}

void main() {
    vec4 pos = vec4(in_v0.xyz, 1.0);
    vec4 cur = vecTimesMat4(vecTimesMat4(pos, mat4(c[0], c[1], c[2], c[3])), mat4(c[4], c[5], c[6], c[7]));
#ifdef GX_VEL_SKIN
    vec4 prv = vecTimesMat4(vec4(in_v1.xyz, 1.0), mat4(c[8], c[9], c[10], c[11]));
#else
    vec4 prv = vecTimesMat4(pos, mat4(c[8], c[9], c[10], c[11]));
#endif
    gl_Position = cur;
    gl_Position.z -= c[12].z * cur.w;            // zEps (default 0): a calibration knob for a depth tie toward the camera
    vec2 curUnjittered = cur.xy - c[13].xy * cur.w;
    out_Texcoord6 = vec4(prv.xy * c[12].xy, 0.0, prv.w);
    out_Texcoord7 = vec4(curUnjittered * c[12].xy, 0.0, cur.w);
    out_Normal = vec4(0.0);
    out_Texcoord0 = vec4(0.0); out_Texcoord1 = vec4(0.0); out_Texcoord2 = vec4(0.0); out_Texcoord3 = vec4(0.0);
    out_Texcoord4 = vec4(0.0); out_Texcoord5 = vec4(0.0);
    out_Color0 = vec4(0.0); out_Color1 = vec4(0.0);
    out_Fog = 0.0;
}
