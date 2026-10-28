#pragma once
// BC1 (DXT1) to ASTC 4x4 LDR transcoding, block by block, without an encoder.
//
// A GPU that cannot sample BC formats (ARM Mali) gets DXT1 textures decoded to 16 bits per texel.
// ASTC 4x4 is 8 bits per texel and a Mali samples it natively, so the same texels cost half the
// memory and half the bandwidth, with full 8-bit colour. A BC1 block is two RGB565 endpoints and
// sixteen 2-bit selectors, which an ASTC block can hold directly:
//
//   one partition, colour endpoint mode 8 (RGB, direct), eight bits per endpoint value
//   four-colour block   block mode 66: 4x4 weights, 2-bit weights (unquantised 0, 21, 43, 64 of 64)
//   three-colour block  block mode 82: 4x4 weights, 5 levels (0, 16, 32, 48, 64 of 64, quint coded)
//
// The four-colour palette (c0, c1, (2*c0+c1)/3, (c0+2*c1)/3) lies on the line c0..c1 at 0, 1, 1/3
// and 2/3, which the weights 0, 64, 21 and 43 reproduce to within two levels. The three-colour
// palette (c0, c1, (c0+c1)/2) is exact with the five-level weights 0, 64 and 32. Blocks that use the
// transparent fourth colour need an alpha channel and are reported, so that the caller can keep
// such a texture in its 16-bit form.
//
// Layout (astc-encoder, symbolic_to_physical): bits 0-10 block mode, 11-12 partition count - 1,
// 13-16 colour endpoint mode, 17.. the endpoint values, and the weights bit-reversed at the top.
// The quint table is the one in astc-encoder (Apache License 2.0, Arm Limited).

#include <cstdint>
#include <cstring>
#include <utility>

namespace rex::graphics::gta4_native {

enum class AstcDxt1Result : uint8_t {
  kOk,            // the block decodes to the BC1 colours within two levels
  kPunchThrough,  // uses the transparent colour: needs alpha, not representable here
};

namespace astc_detail {

// Packed quint value of the triple (hi, mid, lo): index [hi][mid][lo].
inline constexpr uint8_t kIntegerOfQuints[125] = {
    0, 1, 2, 3, 4,
    8, 9, 10, 11, 12,
    16, 17, 18, 19, 20,
    24, 25, 26, 27, 28,
    5, 13, 21, 29, 6,
    32, 33, 34, 35, 36,
    40, 41, 42, 43, 44,
    48, 49, 50, 51, 52,
    56, 57, 58, 59, 60,
    37, 45, 53, 61, 14,
    64, 65, 66, 67, 68,
    72, 73, 74, 75, 76,
    80, 81, 82, 83, 84,
    88, 89, 90, 91, 92,
    69, 77, 85, 93, 22,
    96, 97, 98, 99, 100,
    104, 105, 106, 107, 108,
    112, 113, 114, 115, 116,
    120, 121, 122, 123, 124,
    101, 109, 117, 125, 30,
    102, 103, 70, 71, 38,
    110, 111, 78, 79, 46,
    118, 119, 86, 87, 54,
    126, 127, 94, 95, 62,
    39, 47, 55, 63, 31,
};

inline uint8_t BitReverse8(uint8_t v) {
  v = uint8_t(((v & 0x0F) << 4) | ((v >> 4) & 0x0F));
  v = uint8_t(((v & 0x33) << 2) | ((v >> 2) & 0x33));
  v = uint8_t(((v & 0x55) << 1) | ((v >> 1) & 0x55));
  return v;
}

inline void PutBits(uint8_t* block, uint32_t position, uint32_t count, uint32_t value) {
  for (uint32_t i = 0; i < count; ++i) {
    if ((value >> i) & 1u) block[(position + i) >> 3] |= uint8_t(1u << ((position + i) & 7u));
  }
}

inline void Expand565(uint32_t color, int rgb[3]) {
  const int r = int((color >> 11) & 31), g = int((color >> 5) & 63), b = int(color & 31);
  rgb[0] = (r << 3) | (r >> 2);
  rgb[1] = (g << 2) | (g >> 4);
  rgb[2] = (b << 3) | (b >> 2);
}

// Writes the block. e0/e1 are the endpoints as the weights see them (weight 0 and weight 64) and q
// holds each texel's weight index: 0..3 (2-bit weights) or 0..4 (five levels). The decoder applies
// blue contraction when the first endpoint's channel sum is the larger one, so the endpoints are
// swapped (and the weights mirrored) to keep the sum of e0 no greater than that of e1.
inline void PackAstcBlock(const int e0_in[3], const int e1_in[3], const uint8_t q_in[16],
                          bool five_levels, uint8_t out[16]) {
  const int* e0 = e0_in;
  const int* e1 = e1_in;
  uint8_t q[16];
  std::memcpy(q, q_in, 16);
  if (e0[0] + e0[1] + e0[2] > e1[0] + e1[1] + e1[2]) {
    std::swap(e0, e1);
    for (int i = 0; i < 16; ++i) q[i] = uint8_t((five_levels ? 4 : 3) - q[i]);
  }
  uint8_t weights[16] = {};
  uint32_t weight_bits = 0;
  if (five_levels) {
    // Integer sequence encoding of 16 values with five levels: five groups of three values in 7
    // bits each (the quint code), then the last value alone in 3 bits.
    uint32_t position = 0;
    auto put = [&](uint32_t count, uint32_t value) {
      for (uint32_t i = 0; i < count; ++i, ++position) {
        if ((value >> i) & 1u) weights[position >> 3] |= uint8_t(1u << (position & 7u));
      }
    };
    for (int group = 0; group < 5; ++group) {
      const uint8_t t = kIntegerOfQuints[q[group * 3 + 2] * 25 + q[group * 3 + 1] * 5 + q[group * 3]];
      put(3, t & 7u);
      put(2, (t >> 3) & 3u);
      put(2, (t >> 5) & 3u);
    }
    put(3, q[15]);
    weight_bits = position;  // 38
  } else {
    for (int i = 0; i < 16; ++i) weights[i >> 2] |= uint8_t(q[i] << ((i & 3) * 2));
    weight_bits = 32;
  }
  (void)weight_bits;
  for (int i = 0; i < 16; ++i) out[i] = BitReverse8(weights[15 - i]);
  PutBits(out, 0, 11, five_levels ? 82 : 66);  // block mode: 4x4 weights, one plane
  PutBits(out, 11, 2, 0);                      // one partition
  PutBits(out, 13, 4, 8);                      // colour endpoint mode 8: RGB, direct
  const int values[6] = {e0[0], e1[0], e0[1], e1[1], e0[2], e1[2]};
  for (int i = 0; i < 6; ++i) PutBits(out, 17 + 8 * i, 8, uint32_t(values[i]));
}

}  // namespace astc_detail

inline AstcDxt1Result TranscodeBc1BlockToAstc4x4(const uint8_t bc1[8], uint8_t astc[16]) {
  using namespace astc_detail;
  const uint32_t c0 = uint32_t(bc1[0]) | (uint32_t(bc1[1]) << 8);
  const uint32_t c1 = uint32_t(bc1[2]) | (uint32_t(bc1[3]) << 8);
  const uint32_t selectors = uint32_t(bc1[4]) | (uint32_t(bc1[5]) << 8) |
                             (uint32_t(bc1[6]) << 16) | (uint32_t(bc1[7]) << 24);
  int p0[3], p1[3];
  Expand565(c0, p0);
  Expand565(c1, p1);
  uint8_t q[16];
  if (c0 > c1) {
    // BC1 index 0, 1, 2, 3 = c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1 -> weight index 0, 3, 1, 2
    static const uint8_t kFour[4] = {0, 3, 1, 2};
    for (int i = 0; i < 16; ++i) q[i] = kFour[(selectors >> (i * 2)) & 3];
    PackAstcBlock(p0, p1, q, false, astc);
    return AstcDxt1Result::kOk;
  }
  // Three-colour block: index 0, 1, 2 = c0, c1, the middle -> weight index 0, 4, 2 of five levels.
  // Index 3 is transparent black.
  static const uint8_t kThree[4] = {0, 4, 2, 0};
  for (int i = 0; i < 16; ++i) {
    const uint32_t index = (selectors >> (i * 2)) & 3;
    if (index == 3) return AstcDxt1Result::kPunchThrough;
    q[i] = kThree[index];
  }
  PackAstcBlock(p0, p1, q, true, astc);
  return AstcDxt1Result::kOk;
}

}  // namespace rex::graphics::gta4_native
