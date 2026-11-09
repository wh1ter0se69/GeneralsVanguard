#version 450

// gx_present.vert - ADAPTED from DXVK src/dxvk/shaders/dxvk_present_vert.vert (954c5c692; zlib/libpng, notice in
// gx_present_common.glsl and shaders/SOURCE.txt). Not altered.

layout(location = 0) out vec2 o_coord;

void main() {
  vec2 coord = vec2(
    float(gl_VertexIndex & 2),
    float(gl_VertexIndex & 1) * 2.0f);

  o_coord = coord;
  gl_Position = vec4(-1.0f + 2.0f * coord, 0.0f, 1.0f);
}
