// Round-trip test of native_astc_transcode.h: random BC1 blocks go through the transcoder and a small
// ASTC decoder (written from the astc-encoder decode path) and are compared with the BC1 decode the
// renderer uses. Build and run: g++ -O2 -std=c++17 astc_transcode_test.cpp -o astc_test && ./astc_test
// Expected: no undecodable block, maximum error 2 (four-colour) and 1 (three-colour).
#include "../../../glue/rexglue-sdk-main/src/graphics/gta4_native/native_astc_transcode.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <algorithm>
using namespace rex::graphics::gta4_native;

// --- reference BC1 decode, copied from conversion.cpp (DecodeBC1Colors)
static void RefBC1(const uint8_t* block, bool three, uint8_t out[64]) {
  const uint32_t c0 = uint32_t(block[0]) | (uint32_t(block[1]) << 8);
  const uint32_t c1 = uint32_t(block[2]) | (uint32_t(block[3]) << 8);
  uint8_t palette[4][4];
  auto expand = [](uint32_t color, uint8_t* rgba) {
    const uint32_t r = (color >> 11) & 31, g = (color >> 5) & 63, b = color & 31;
    rgba[0] = uint8_t((r << 3) | (r >> 2)); rgba[1] = uint8_t((g << 2) | (g >> 4)); rgba[2] = uint8_t((b << 3) | (b >> 2)); rgba[3] = 255;
  };
  expand(c0, palette[0]); expand(c1, palette[1]);
  if (c0 > c1 || !three) {
    for (uint32_t i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((2 * uint32_t(palette[0][i]) + palette[1][i] + 1) / 3);
      palette[3][i] = uint8_t((uint32_t(palette[0][i]) + 2 * palette[1][i] + 1) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (uint32_t i = 0; i < 3; ++i) palette[2][i] = uint8_t((uint32_t(palette[0][i]) + palette[1][i]) / 2);
    palette[2][3] = 255; std::memset(palette[3], 0, 4);
  }
  const uint32_t sel = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
  for (uint32_t p = 0; p < 16; ++p) std::memcpy(out + p * 4, palette[(sel >> (p * 2)) & 3], 4);
}

// --- minimal ASTC decoder for exactly the blocks this transcoder emits (transcribed from the
// astc-encoder decode path: physical_to_symbolic, rgb_unpack/rgba_unpack, weight interpolation)
static bool DecodeAstc(const uint8_t pcb[16], uint8_t out[64]) {
  auto read_bits = [&](int count, int pos) { uint32_t v = 0; for (int i = 0; i < count; ++i) v |= ((pcb[(pos + i) >> 3] >> ((pos + i) & 7)) & 1u) << i; return v; };
  const uint32_t mode = read_bits(11, 0);
  if ((mode != 66 && mode != 82) || read_bits(2, 11) != 0 || read_bits(4, 13) != 8) return false;
  uint8_t sw[16];
  for (int i = 0; i < 16; ++i) sw[i] = astc_detail::BitReverse8(pcb[15 - i]);
  auto sbits = [&](int count, int pos) { uint32_t v = 0; for (int i = 0; i < count; ++i) v |= ((sw[(pos + i) >> 3] >> ((pos + i) & 7)) & 1u) << i; return v; };
  int w[16];
  if (mode == 66) {
    static const int unq[4] = {0, 21, 43, 64};
    for (int i = 0; i < 16; ++i) w[i] = unq[sbits(2, i * 2)];
  } else {
    static const int unq[5] = {0, 16, 32, 48, 64};
    int idx[16]; int pos = 0;
    // decode: groups of three values from a 7-bit code (inverse of the quint table), then one value
    auto triple = [&](uint32_t T, int* o) {
      for (int hi = 0; hi < 5; ++hi) for (int mid = 0; mid < 5; ++mid) for (int lo = 0; lo < 5; ++lo)
        if (astc_detail::kIntegerOfQuints[hi * 25 + mid * 5 + lo] == T) { o[0] = lo; o[1] = mid; o[2] = hi; return; }
    };
    for (int g = 0; g < 5; ++g) {
      uint32_t T = sbits(3, pos) | (sbits(2, pos + 3) << 3) | (sbits(2, pos + 5) << 5); pos += 7;
      triple(T, &idx[g * 3]);
    }
    idx[15] = int(sbits(3, pos)); pos += 3;
    if (pos != 38) return false;
    for (int i = 0; i < 16; ++i) { if (idx[i] > 4) return false; w[i] = unq[idx[i]]; }
  }
  int v[6];
  for (int i = 0; i < 6; ++i) v[i] = int(read_bits(8, 17 + 8 * i));
  int e0[3] = {v[0], v[2], v[4]}, e1[3] = {v[1], v[3], v[5]};
  if (e0[0] + e0[1] + e0[2] > e1[0] + e1[1] + e1[2]) {  // blue contraction
    int t0[3] = {(e0[0] + e0[2]) >> 1, (e0[1] + e0[2]) >> 1, e0[2]};
    int t1[3] = {(e1[0] + e1[2]) >> 1, (e1[1] + e1[2]) >> 1, e1[2]};
    for (int k = 0; k < 3; ++k) { e0[k] = t1[k]; e1[k] = t0[k]; }
  }
  for (int i = 0; i < 16; ++i) {
    for (int k = 0; k < 3; ++k) {
      const int c0 = e0[k] * 257, c1 = e1[k] * 257;
      const int c = (c0 * (64 - w[i]) + c1 * w[i] + 32) >> 6;
      out[i * 4 + k] = uint8_t((c * 255 + 32767) / 65535);
    }
    out[i * 4 + 3] = 255;
  }
  return true;
}

int main() {
  std::mt19937 rng(12345);
  long blocks = 0, punch = 0, inexact = 0, bad_decode = 0;
  int max_err_4 = 0, max_err_3 = 0; long err_hist[8] = {};
  const int N = 4000000;
  for (int n = 0; n < N; ++n) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = uint8_t(rng());
    // bias: half of the blocks smooth (close endpoints), a quarter forced three-colour
    if (n % 2) {
      uint16_t c0 = uint16_t(rng()), d = uint16_t(rng() % 4096);
      uint16_t c1 = uint16_t(c0 + d); if (n % 4 == 1 && c1 > c0) std::swap(c0, c1);
      b[0] = c0 & 255; b[1] = c0 >> 8; b[2] = c1 & 255; b[3] = c1 >> 8;
    }
    if (n % 3 == 0) {  // three-colour block with indices 0..2 only
      uint32_t s = 0; for (int i = 0; i < 16; ++i) s |= uint32_t(rng() % 3) << (i * 2);
      std::memcpy(b + 4, &s, 4);
      uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
      if (c0 > c1) { std::swap(c0, c1); b[0] = c0 & 255; b[1] = c0 >> 8; b[2] = c1 & 255; b[3] = c1 >> 8; }
    }
    uint8_t a[16]; uint8_t ref[64], got[64];
    const AstcDxt1Result r = TranscodeBc1BlockToAstc4x4(b, a);
    ++blocks;
    if (r == AstcDxt1Result::kPunchThrough) { ++punch; continue; }
    RefBC1(b, true, ref);
    if (!DecodeAstc(a, got)) { ++bad_decode; continue; }
    int worst = 0;
    for (int i = 0; i < 64; ++i) worst = std::max(worst, std::abs(int(ref[i]) - int(got[i])));
    const uint32_t c0 = uint32_t(b[0]) | (uint32_t(b[1]) << 8), c1 = uint32_t(b[2]) | (uint32_t(b[3]) << 8);
        if (c0 > c1) max_err_4 = std::max(max_err_4, worst); else max_err_3 = std::max(max_err_3, worst);
    err_hist[std::min(worst, 6)]++;
  }
  std::printf("blocks %ld  punch-through %ld  inexact %ld  undecodable %ld\n", blocks, punch, inexact, bad_decode);
  std::printf("max error four-colour %d  three-colour %d\n", max_err_4, max_err_3);
  for (int i = 0; i < 7; ++i) std::printf("worst=%d%s : %ld\n", i, i == 6 ? "+" : "", err_hist[i]);
  std::printf("inexact blocks with error > 3: %ld\n", err_hist[7]);
}
