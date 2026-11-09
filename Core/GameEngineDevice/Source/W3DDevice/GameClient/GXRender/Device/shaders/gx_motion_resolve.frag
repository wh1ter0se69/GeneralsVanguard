#version 450

// gx_motion_resolve.frag - GeneralsX @feature E · render-motion-1 13/10/2026: camera motion vectors from the world depth
// (modernize/render-dlss/DESIGN_motion.md §6.9). One fragment per world pixel p: the un-jittered position of the surface the
// jittered raster put at p is u = p - jitter; its clip position (NDC from the world viewport, z from the depth buffer) goes
// through clipToPrevClip; mv = previous pixel - u = CURRENT -> PREVIOUS in render pixels, +x right / +y down (GxMotionFrame.h).

layout(set = 0, binding = 0) uniform sampler2D s_depth;   // DEPTH aspect, single sample (texelFetch: the sampler is unused)
// render-motion-units-1's object vectors (RGBA16F, same convention; a > 0.5 = written); the depth again when none (flags.x = 0)
layout(set = 0, binding = 1) uniform sampler2D s_object;

layout(push_constant) uniform Push {
  // D3D row-vector matrix pushed row-major: GLSL reads its transpose, so clipToPrevClip * v here == v * M in D3D terms
  mat4 clipToPrevClip;
  vec4 viewport;     // xy = (W/2, H/2) of the world viewport; zw = its centre in framebuffer pixels (incl. D3D9's 0.5 offset)
  vec4 jitterDepth;  // xy = jitter in pixels (+x right, +y down); z = viewport minDepth; w = 1 / (maxDepth - minDepth)
  vec4 flags;        // x = 1: s_object holds this pass's object vectors
} pc;

layout(location = 0) out vec2 o_mv;

void main() {
  if (pc.flags.x > 0.5) {
    vec4 o = texelFetch(s_object, ivec2(gl_FragCoord.xy), 0);
    if (o.a > 0.5) {   // a moved / skinned object drew here: its own vector wins over the camera's
      o_mv = o.xy;
      return;
    }
  }
  vec2 u = gl_FragCoord.xy - pc.jitterDepth.xy;
  vec2 ndc = (u - pc.viewport.zw) / vec2(pc.viewport.x, -pc.viewport.y);
  float d = texelFetch(s_depth, ivec2(gl_FragCoord.xy), 0).r;
  vec4 prev = pc.clipToPrevClip * vec4(ndc, (d - pc.jitterDepth.z) * pc.jitterDepth.w, 1.0);
  if (prev.w <= 1e-6) {   // behind the previous camera: no usable history
    o_mv = vec2(0.0);
    return;
  }
  vec2 prevPx = prev.xy / prev.w * vec2(pc.viewport.x, -pc.viewport.y) + pc.viewport.zw;
  o_mv = prevPx - u;
}
