#version 450
// gx_cs_vel.frag - gx:vel-ps (Pipeline/GxSyntheticPrograms.h; D · render-motion-units-1 13/10/2026, DESIGN_motion-units.md §2.7),
// the object-velocity fragment program of the DLSS-track motion bridge. Its own module (not a case of gx_cs_ps.frag), so every
// pre-existing custom pipeline keeps a byte-identical SPIR-V module. Same pipeline layout as every custom program; it reads only
// the two varyings gx_cs_vel.vert writes (previous / current clip position pre-scaled to pixels, at the FF texcoord 6 / 7 locations).
// Output: current -> previous in render pixels, +y down, A = 1 (written). The per-pixel perspective divide of each interpolated
// clip position is the surface point's projected position in that frame; the viewport's half-pixel offset cancels in the difference.
layout(location = 7) in vec4 in_Texcoord6;
layout(location = 8) in vec4 in_Texcoord7;
layout(location = 0) out vec4 out_Color0;

void main() {
    out_Color0 = vec4(in_Texcoord6.xy / in_Texcoord6.w - in_Texcoord7.xy / in_Texcoord7.w, 0.0, 1.0);
}
