// DXT5 block compression (see dxt.h). A simple, fast encoder: the alpha
// end points are the block's minimum and maximum (eight-step mode), the
// colour end points the corners of the block's colour bounding box pulled in
// by 1/16 (the usual inset, so the end colours sit on more pixels), and every
// pixel takes the nearest of the four (or eight) palette entries.
#include "dxt.h"

namespace {

inline uint32_t channel(uint32_t argb, int shift) { return (argb >> shift) & 0xFF; }

inline uint16_t to565(uint32_t r, uint32_t g, uint32_t b) {
  return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

inline void from565(uint16_t c, uint32_t* r, uint32_t* g, uint32_t* b) {
  const uint32_t r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

void alpha_block(const uint32_t pixels[16], uint8_t out[8]) {
  uint32_t lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) {
    const uint32_t a = channel(pixels[i], 24);
    if (a < lo) lo = a;
    if (a > hi) hi = a;
  }
  out[0] = (uint8_t)hi;
  out[1] = (uint8_t)lo;
  uint64_t bits = 0;
  if (hi != lo) {
    // Palette (hi > lo): hi, lo, then six steps from hi to lo; codes 0 and 1
    // are the ends, 2..7 the steps in between.
    uint32_t palette[8];
    palette[0] = hi;
    palette[1] = lo;
    for (uint32_t k = 1; k <= 6; ++k) palette[1 + k] = ((7 - k) * hi + k * lo) / 7;
    for (int i = 0; i < 16; ++i) {
      const uint32_t a = channel(pixels[i], 24);
      uint32_t best = 0, best_err = 1000;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t err = a > palette[k] ? a - palette[k] : palette[k] - a;
        if (err < best_err) {
          best_err = err;
          best = k;
        }
      }
      bits |= (uint64_t)best << (3 * i);
    }
  }
  for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bits >> (8 * i));
}

void colour_block(const uint32_t pixels[16], uint8_t out[8]) {
  uint32_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  const int shifts[3] = {16, 8, 0};
  for (int i = 0; i < 16; ++i) {
    for (int c = 0; c < 3; ++c) {
      const uint32_t v = channel(pixels[i], shifts[c]);
      if (v < lo[c]) lo[c] = v;
      if (v > hi[c]) hi[c] = v;
    }
  }
  for (int c = 0; c < 3; ++c) {
    const uint32_t inset = (hi[c] - lo[c]) / 16;
    lo[c] += inset;
    hi[c] -= inset;
  }
  uint16_t c0 = to565(hi[0], hi[1], hi[2]), c1 = to565(lo[0], lo[1], lo[2]);
  uint32_t indices = 0;
  if (c0 == c1) {
    // One colour: four-colour mode needs c0 > c1, so nudge c1 down; every
    // pixel uses c0.
    if (c1 > 0) --c1;
    else ++c0;
    indices = 0;
  } else {
    if (c0 < c1) {
      const uint16_t t = c0;
      c0 = c1;
      c1 = t;
    }
    uint32_t pr[4], pg[4], pb[4];
    from565(c0, &pr[0], &pg[0], &pb[0]);
    from565(c1, &pr[1], &pg[1], &pb[1]);
    pr[2] = (2 * pr[0] + pr[1]) / 3, pg[2] = (2 * pg[0] + pg[1]) / 3, pb[2] = (2 * pb[0] + pb[1]) / 3;
    pr[3] = (pr[0] + 2 * pr[1]) / 3, pg[3] = (pg[0] + 2 * pg[1]) / 3, pb[3] = (pb[0] + 2 * pb[1]) / 3;
    for (int i = 0; i < 16; ++i) {
      const int r = (int)channel(pixels[i], 16), g = (int)channel(pixels[i], 8), b = (int)channel(pixels[i], 0);
      uint32_t best = 0;
      int best_err = 1 << 30;
      for (uint32_t k = 0; k < 4; ++k) {
        const int dr = r - (int)pr[k], dg = g - (int)pg[k], db = b - (int)pb[k];
        const int err = dr * dr + dg * dg + db * db;
        if (err < best_err) {
          best_err = err;
          best = k;
        }
      }
      indices |= best << (2 * i);
    }
  }
  out[0] = (uint8_t)c0;
  out[1] = (uint8_t)(c0 >> 8);
  out[2] = (uint8_t)c1;
  out[3] = (uint8_t)(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[4 + i] = (uint8_t)(indices >> (8 * i));
}

}  // namespace

void gw_dxt5_block(const uint32_t pixels[16], uint8_t out[16]) {
  alpha_block(pixels, out);
  colour_block(pixels, out + 8);
}

void gw_dxt5_decode_block(const uint8_t in[16], uint32_t pixels[16]) {
  uint32_t alpha[8];
  alpha[0] = in[0];
  alpha[1] = in[1];
  if (alpha[0] > alpha[1]) {
    for (uint32_t k = 1; k <= 6; ++k) alpha[1 + k] = ((7 - k) * alpha[0] + k * alpha[1]) / 7;
  } else {
    for (uint32_t k = 1; k <= 4; ++k) alpha[1 + k] = ((5 - k) * alpha[0] + k * alpha[1]) / 5;
    alpha[6] = 0;
    alpha[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= (uint64_t)in[2 + i] << (8 * i);
  const uint16_t c0 = (uint16_t)(in[8] | in[9] << 8), c1 = (uint16_t)(in[10] | in[11] << 8);
  uint32_t r[4], g[4], b[4];
  from565(c0, &r[0], &g[0], &b[0]);
  from565(c1, &r[1], &g[1], &b[1]);
  r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, b[2] = (2 * b[0] + b[1]) / 3;
  r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, b[3] = (b[0] + 2 * b[1]) / 3;
  const uint32_t cbits = (uint32_t)(in[12] | in[13] << 8 | in[14] << 16 | (uint32_t)in[15] << 24);
  for (int i = 0; i < 16; ++i) {
    const uint32_t a = alpha[(abits >> (3 * i)) & 7], k = (cbits >> (2 * i)) & 3;
    pixels[i] = a << 24 | r[k] << 16 | g[k] << 8 | b[k];
  }
}
