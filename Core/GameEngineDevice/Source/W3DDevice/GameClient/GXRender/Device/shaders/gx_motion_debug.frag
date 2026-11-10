#version 450

// gx_motion_debug.frag - GeneralsX: GX_MOTION_DEBUG views drawn over the world colour at
// IGxWorldPass::End (DESIGN_motion.md §4, §6.7). Mode 0 = linearised depth as grey; mode 1 = motion vectors as HSV (hue =
// direction, brightness = magnitude, full at pc.p.x pixels; black = no motion).

layout(constant_id = 0) const int c_mode = 0;

layout(set = 0, binding = 0) uniform sampler2D s_src;   // depth (DEPTH aspect) or the mv image

layout(push_constant) uniform Push {
  vec4 p;   // depth: x = near, y = far, z = viewport minDepth, w = 1 / (maxDepth - minDepth); velocity: x = full-scale pixels
} pc;

layout(location = 0) out vec4 o_color;

vec3 hsv2rgb(vec3 c) {
  vec4 k = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
  vec3 q = abs(fract(c.xxx + k.xyz) * 6.0 - k.www);
  return c.z * mix(k.xxx, clamp(q - k.xxx, 0.0, 1.0), c.y);
}

void main() {
  vec4 t = texelFetch(s_src, ivec2(gl_FragCoord.xy), 0);
  if (c_mode == 0) {
    float z = (t.r - pc.p.z) * pc.p.w;
    float n = pc.p.x, f = pc.p.y;
    float vz = n * f / max(f - z * (f - n), 1e-6);   // D3D perspective depth -> view distance
    o_color = vec4(vec3(sqrt(clamp((vz - n) / (f - n), 0.0, 1.0))), 1.0);
  } else {
    float m = length(t.xy);
    float h = atan(t.y, t.x) / 6.2831853 + 0.5;
    o_color = m > 0.01 ? vec4(hsv2rgb(vec3(h, 1.0, 0.25 + 0.75 * clamp(m / pc.p.x, 0.0, 1.0))), 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
  }
}
