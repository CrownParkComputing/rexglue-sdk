#version 450
// Pack a resolved host depth/stencil attachment into the guest's D24S8 bytes.
// MCLA fetches these bytes as 8_8_8_8 to read the object index from stencil.
layout(set = 0, binding = 0) uniform sampler2D depth_source;
layout(set = 0, binding = 1) uniform usampler2D stencil_source;
layout(push_constant) uniform Push {
  ivec2 src_offset;
  ivec2 format;  // x: 0 = D24S8, 1 = D24FS8
  vec4 scale;
} pc;
layout(location = 0) out vec4 o;

// Same rounding and exponent bias as xenos::Float32To20e4.
uint pack_float24(float depth) {
  if (!(depth > 0.0)) return 0u;
  uint bits = floatBitsToUint(depth);
  if (bits >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (bits < 0x38800000u) {
    uint shift = min(113u - (bits >> 23), 24u);
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  bits += 3u + ((bits >> 3) & 1u);
  return (bits >> 3) & 0xFFFFFFu;
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) + pc.src_offset;
  float depth = texelFetch(depth_source, p, 0).r;
  uint d = pc.format.x != 0 ? pack_float24(depth)
                           : uint(roundEven(clamp(depth, 0.0, 1.0) * 16777215.0));
  uint s = texelFetch(stencil_source, p, 0).r;
  uint packed = (d << 8) | (s & 255u);
  o = vec4(uvec4(packed, packed >> 8, packed >> 16, packed >> 24) & 255u) / 255.0;
}
