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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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

// ---------------------------------------------------------------------------------------------
// BC2 / BC3 (DXT3 / DXT5) with simple alpha: ASTC 4x4 with two weight planes.
//
// A 128-bit block has to hold colour and alpha, so the weights are two 2-bit planes (block mode
// 1090: 4x4 weights, dual plane, 4 levels) and the eight RGBA endpoint values (CEM 12) get the 45 bits
// that are left, which the decoder reads as 48 levels (4 bits and a trit each). Endpoints are cut to
// the nearest of those 48 values (steps of about 5 of 255). The colour weights are the BC1 selectors
// (four-colour mode, as BC2/BC3 always use).
// Alpha is refitted: the block's smallest and largest alpha become the endpoints and each texel takes
// the nearest of four levels in between. That is exact for the common cut-out alpha (all 0 or all
// 255, fences, leaves) and approximate for gradients, so the caller keeps a texture in RGBA8 when
// too many blocks fit badly (alpha used as a data channel, noisy alpha).
// ---------------------------------------------------------------------------------------------
struct AstcAlphaFit {
  uint32_t max_error;      // largest alpha difference to the BC block, 0..255
  uint32_t squared_error;  // sum over the 16 texels
};

namespace astc_detail {

inline constexpr uint8_t kIntegerOfTrits[243] = {
    0, 1, 2, 4, 5, 6, 8, 9, 10, 16, 17, 18, 20, 21, 22, 24, 25, 26, 3, 7, 15, 19, 23, 27, 12, 13, 14,
    32, 33, 34, 36, 37, 38, 40, 41, 42, 48, 49, 50, 52, 53, 54, 56, 57, 58, 35, 39, 47, 51, 55, 59, 44, 45, 46,
    64, 65, 66, 68, 69, 70, 72, 73, 74, 80, 81, 82, 84, 85, 86, 88, 89, 90, 67, 71, 79, 83, 87, 91, 76, 77, 78,
    128, 129, 130, 132, 133, 134, 136, 137, 138, 144, 145, 146, 148, 149, 150, 152, 153, 154, 131, 135, 143, 147, 151, 155, 140, 141, 142,
    160, 161, 162, 164, 165, 166, 168, 169, 170, 176, 177, 178, 180, 181, 182, 184, 185, 186, 163, 167, 175, 179, 183, 187, 172, 173, 174,
    192, 193, 194, 196, 197, 198, 200, 201, 202, 208, 209, 210, 212, 213, 214, 216, 217, 218, 195, 199, 207, 211, 215, 219, 204, 205, 206,
    96, 97, 98, 100, 101, 102, 104, 105, 106, 112, 113, 114, 116, 117, 118, 120, 121, 122, 99, 103, 111, 115, 119, 123, 108, 109, 110,
    224, 225, 226, 228, 229, 230, 232, 233, 234, 240, 241, 242, 244, 245, 246, 248, 249, 250, 227, 231, 239, 243, 247, 251, 236, 237, 238,
    28, 29, 30, 60, 61, 62, 92, 93, 94, 156, 157, 158, 188, 189, 190, 220, 221, 222, 31, 63, 127, 159, 191, 255, 252, 253, 254,
};
inline constexpr uint8_t kPackedOfTarget48[256] = {
    0, 0, 0, 16, 16, 16, 16, 16, 32, 32, 32, 32, 32, 32, 2, 2,
    2, 2, 2, 18, 18, 18, 18, 18, 34, 34, 34, 34, 34, 34, 4, 4,
    4, 4, 4, 20, 20, 20, 20, 20, 20, 36, 36, 36, 36, 36, 6, 6,
    6, 6, 6, 22, 22, 22, 22, 22, 22, 38, 38, 38, 38, 38, 38, 8,
    8, 8, 8, 8, 24, 24, 24, 24, 24, 40, 40, 40, 40, 40, 40, 10,
    10, 10, 10, 10, 26, 26, 26, 26, 26, 42, 42, 42, 42, 42, 42, 12,
    12, 12, 12, 12, 28, 28, 28, 28, 28, 28, 44, 44, 44, 44, 44, 14,
    14, 14, 14, 14, 30, 30, 30, 30, 30, 30, 46, 46, 46, 46, 46, 46,
    47, 47, 47, 47, 47, 47, 31, 31, 31, 31, 31, 31, 15, 15, 15, 15,
    15, 45, 45, 45, 45, 45, 29, 29, 29, 29, 29, 29, 13, 13, 13, 13,
    13, 43, 43, 43, 43, 43, 43, 27, 27, 27, 27, 27, 11, 11, 11, 11,
    11, 41, 41, 41, 41, 41, 41, 25, 25, 25, 25, 25, 9, 9, 9, 9,
    9, 39, 39, 39, 39, 39, 39, 23, 23, 23, 23, 23, 23, 7, 7, 7,
    7, 7, 37, 37, 37, 37, 37, 21, 21, 21, 21, 21, 21, 5, 5, 5,
    5, 5, 35, 35, 35, 35, 35, 35, 19, 19, 19, 19, 19, 3, 3, 3,
    3, 3, 33, 33, 33, 33, 33, 33, 17, 17, 17, 17, 17, 1, 1, 1,
};

// The 8-bit value the decoder produces for each of the 48 endpoint codes (4 bits + trit), from
// astc-encoder's unquantisation, found by decoding blocks with the reference decoder.
inline constexpr uint8_t kDecoded48[48] = {0,255,16,239,32,223,48,207,65,190,81,174,97,158,113,142,5,250,21,234,38,217,54,201,70,185,86,169,103,152,119,136,11,244,27,228,43,212,59,196,76,179,92,163,108,147,124,131};

inline int Code48(int value) { return kPackedOfTarget48[std::clamp(value, 0, 255)]; }

// Eight endpoint values of 4 bits + one trit each (integer sequence encoding) at bit `at`.
inline void PutEndpoints48(uint8_t* block, uint32_t at, const uint8_t v[8]) {
  uint32_t position = at;
  auto put = [&](uint32_t value, uint32_t count) { PutBits(block, position, count, value); position += count; };
  const auto trit = [&](int i) { return uint32_t(v[i] >> 4); };
  const uint8_t t = kIntegerOfTrits[trit(4) * 81 + trit(3) * 27 + trit(2) * 9 + trit(1) * 3 + trit(0)];
  put((v[0] & 15u) | ((t & 3u) << 4), 6);
  put((v[1] & 15u) | (((t >> 2) & 3u) << 4), 6);
  put((v[2] & 15u) | (((t >> 4) & 1u) << 4), 5);
  put((v[3] & 15u) | (((t >> 5) & 3u) << 4), 6);
  put((v[4] & 15u) | (((t >> 7) & 1u) << 4), 5);
  const uint8_t u = kIntegerOfTrits[trit(7) * 9 + trit(6) * 3 + trit(5)];
  put((v[5] & 15u) | ((u & 3u) << 4), 6);
  put((v[6] & 15u) | (((u >> 2) & 3u) << 4), 6);
  put((v[7] & 15u) | (((u >> 4) & 1u) << 4), 5);
}

}  // namespace astc_detail

// One BC4 block (the alpha half of BC3, each channel of BC5/DXN) as the renderer's own decoder gives it.
inline void DecodeBc4Block(const uint8_t block[8], int out[16]) {
  int palette[8] = {block[0], block[1]};
  if (block[0] > block[1]) {
    for (int k = 2; k < 8; ++k) palette[k] = ((8 - k) * block[0] + (k - 1) * block[1]) / 7;
  } else {
    for (int k = 2; k < 6; ++k) palette[k] = ((6 - k) * block[0] + (k - 1) * block[1]) / 5;
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t selectors = 0;
  for (int b = 0; b < 6; ++b) selectors |= uint64_t(block[b + 2]) << (b * 8);
  for (int i = 0; i < 16; ++i) out[i] = palette[(selectors >> (i * 3)) & 7];
}

namespace astc_detail {

inline constexpr int kLevel4[4] = {0, 21, 43, 64};  // the 2-bit weights, in 64ths

// Fits sixteen values with four evenly spaced levels between two endpoints. Starts from the extremes
// (exact for cut-outs and two-tone data), then refines the endpoints by least squares a few times and
// keeps the best by squared error. With `coarse` the endpoints are limited to the 48 values of the
// RGBA path (kDecoded48), otherwise to 8 bits. `lo_code`/`hi_code` are what goes into the block.
struct Fit4 {
  int lo_code = 0, hi_code = 0;
  uint8_t weight[16] = {};
  uint32_t max_error = 0, squared_error = ~0u;
};

inline Fit4 FitFourLevels(const int v[16], bool coarse) {
  const auto evaluate = [&](float lo_f, float hi_f) {
    Fit4 fit;
    const int lo_wanted = std::clamp(int(lo_f + 0.5f), 0, 255), hi_wanted = std::clamp(int(hi_f + 0.5f), 0, 255);
    fit.lo_code = coarse ? Code48(lo_wanted) : lo_wanted;
    fit.hi_code = coarse ? Code48(hi_wanted) : hi_wanted;
    const int low = coarse ? kDecoded48[fit.lo_code] : fit.lo_code;
    const int high = coarse ? kDecoded48[fit.hi_code] : fit.hi_code;
    fit.squared_error = 0;
    for (int i = 0; i < 16; ++i) {
      int best = 0, best_error = 1 << 20;
      for (int k = 0; k < 4; ++k) {
        const int value = (low * (64 - kLevel4[k]) + high * kLevel4[k] + 32) >> 6;
        const int error = std::abs(value - v[i]);
        if (error < best_error) { best_error = error; best = k; }
      }
      fit.weight[i] = uint8_t(best);
      fit.max_error = std::max(fit.max_error, uint32_t(best_error));
      fit.squared_error += uint32_t(best_error * best_error);
    }
    return fit;
  };
  int vmin = 255, vmax = 0;
  for (int i = 0; i < 16; ++i) { vmin = std::min(vmin, v[i]); vmax = std::max(vmax, v[i]); }
  Fit4 best = evaluate(float(vmin), float(vmax));
  Fit4 current = best;
  for (int round = 0; round < 3 && best.squared_error != 0; ++round) {
    // Least squares for the two endpoints given each value's level.
    double aa = 0, ab = 0, bb = 0, av = 0, bv = 0;
    for (int i = 0; i < 16; ++i) {
      const double p = kLevel4[current.weight[i]] / 64.0, a = 1.0 - p;
      aa += a * a; ab += a * p; bb += p * p; av += a * v[i]; bv += p * v[i];
    }
    const double det = aa * bb - ab * ab;
    if (det < 1e-6) break;
    const float lo_f = float((av * bb - bv * ab) / det), hi_f = float((aa * bv - ab * av) / det);
    current = evaluate(lo_f, hi_f);
    if (current.squared_error < best.squared_error) best = current;
  }
  return best;
}

}  // namespace astc_detail

// `block` is one BC2 or BC3 block in memory order (alpha half first, colour half second).
inline AstcAlphaFit TranscodeBc3BlockToAstc4x4(const uint8_t block[16], bool explicit_alpha, uint8_t astc[16]) {
  using namespace astc_detail;
  int alpha[16];
  if (explicit_alpha) {
    for (int i = 0; i < 16; ++i) alpha[i] = ((block[i >> 1] >> ((i & 1) * 4)) & 15) * 17;
  } else {
    DecodeBc4Block(block, alpha);
  }

  // Colour: the BC1 endpoints and selectors, endpoints cut to the nearest of the 48 values.
  const uint8_t* colour = block + 8;
  const uint32_t c0 = uint32_t(colour[0]) | (uint32_t(colour[1]) << 8);
  const uint32_t c1 = uint32_t(colour[2]) | (uint32_t(colour[3]) << 8);
  const uint32_t selectors = uint32_t(colour[4]) | (uint32_t(colour[5]) << 8) | (uint32_t(colour[6]) << 16) |
                             (uint32_t(colour[7]) << 24);
  int code[2][3], decoded[2][3];
  for (int e = 0; e < 2; ++e) {
    int expanded[3];
    Expand565(e ? c1 : c0, expanded);
    for (int k = 0; k < 3; ++k) {
      code[e][k] = Code48(expanded[k]);
      decoded[e][k] = kDecoded48[code[e][k]];
    }
  }
  static const uint8_t kFour[4] = {0, 3, 1, 2};
  uint8_t colour_weight[16];
  for (int i = 0; i < 16; ++i) colour_weight[i] = kFour[(selectors >> (i * 2)) & 3];
  const int sum0 = decoded[0][0] + decoded[0][1] + decoded[0][2];
  const int sum1 = decoded[1][0] + decoded[1][1] + decoded[1][2];
  int first = 0, second = 1;
  if (sum0 > sum1) {  // keep the first endpoint's sum the smaller one: no blue contraction
    first = 1;
    second = 0;
    for (int i = 0; i < 16; ++i) colour_weight[i] = uint8_t(3 - colour_weight[i]);
  }

  const Fit4 fit = FitFourLevels(alpha, true);

  // Pack: two interleaved 2-bit weight planes (colour, alpha), bit reversed at the top of the block.
  uint8_t weights[16] = {};
  for (int i = 0; i < 16; ++i) {
    weights[(4 * i) >> 3] |= uint8_t(colour_weight[i] << ((4 * i) & 7));
    weights[(4 * i + 2) >> 3] |= uint8_t(fit.weight[i] << ((4 * i + 2) & 7));
  }
  for (int i = 0; i < 16; ++i) astc[i] = BitReverse8(weights[15 - i]);
  PutBits(astc, 0, 11, 1090);  // block mode: 4x4 weights, dual plane, 2-bit weights
  PutBits(astc, 11, 2, 0);     // one partition
  PutBits(astc, 13, 4, 12);    // colour endpoint mode 12: RGBA, direct
  const uint8_t values[8] = {uint8_t(code[first][0]), uint8_t(code[second][0]), uint8_t(code[first][1]),
                             uint8_t(code[second][1]), uint8_t(code[first][2]), uint8_t(code[second][2]),
                             uint8_t(fit.lo_code), uint8_t(fit.hi_code)};
  PutEndpoints48(astc, 17, values);
  PutBits(astc, 62, 2, 3);  // the second weight plane drives alpha
  return AstcAlphaFit{fit.max_error, fit.squared_error};
}

// One BC5 / DXN block (two BC4 blocks: first and second channel) as ASTC 4x4 luminance + alpha with two
// weight planes: the first channel is the luminance (read back as red), the second the alpha. Both have
// full 8-bit endpoints (four values fit the 45 spare bits), so a smooth normal map loses little. The
// view that samples it swaps the channels back: (red, alpha, 0, 1) = the decoded (R8, G8, 0, 1) texel.
// `first` and `second` are 8-byte BC4 blocks in memory order. Returns the worse of the two channels.
inline AstcAlphaFit TranscodeBc5BlockToAstc4x4(const uint8_t first[8], const uint8_t second[8], uint8_t astc[16]) {
  using namespace astc_detail;
  int a[16], b[16];
  DecodeBc4Block(first, a);
  DecodeBc4Block(second, b);
  const Fit4 fit_a = FitFourLevels(a, false), fit_b = FitFourLevels(b, false);
  uint8_t weights[16] = {};
  for (int i = 0; i < 16; ++i) {
    weights[(4 * i) >> 3] |= uint8_t(fit_a.weight[i] << ((4 * i) & 7));
    weights[(4 * i + 2) >> 3] |= uint8_t(fit_b.weight[i] << ((4 * i + 2) & 7));
  }
  for (int i = 0; i < 16; ++i) astc[i] = BitReverse8(weights[15 - i]);
  PutBits(astc, 0, 11, 1090);
  PutBits(astc, 11, 2, 0);
  PutBits(astc, 13, 4, 4);  // colour endpoint mode 4: luminance + alpha, direct
  PutBits(astc, 17, 8, uint32_t(fit_a.lo_code));
  PutBits(astc, 25, 8, uint32_t(fit_a.hi_code));
  PutBits(astc, 33, 8, uint32_t(fit_b.lo_code));
  PutBits(astc, 41, 8, uint32_t(fit_b.hi_code));
  PutBits(astc, 62, 2, 3);  // the second weight plane drives alpha
  return AstcAlphaFit{std::max(fit_a.max_error, fit_b.max_error), fit_a.squared_error + fit_b.squared_error};
}

}  // namespace rex::graphics::gta4_native
