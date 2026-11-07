// Native replacement for the background starfield update (guest 0x82075548),
// once a frame: 6000 stars, each twinkling, written as point vertices
// (x, y, z, r, g, b; 24 bytes) into one of three rotating vertex buffers
// that vertex shader A1E03FA2 draws.
//
// Star records (20 bytes from 0x821B0000 + 18288): x, y, brightness, twinkle
// phase, z. Each frame the phase runs down by brightness * a + b and starts
// again at 1 when it passes 0; the star's colour is
//   ((brightness + 1) * sin(phase * k)^2 * c + brightness * d + b) * scale * fade
// times the star colour (r, g, b constants), where 'fade' rises to 1 as the
// screen fades in and 'scale' comes from the game's power function.
// The maths follows the PowerPC code in single precision.
#include "runtime.h"

extern "C" void __imp__sub_8212E9B0(PPCContext& ctx, uint8_t* base);  // sine (the original)
extern "C" void sub_821303B8(PPCContext& ctx, uint8_t* base);         // f1 = f(f1, f2): the game's pow
extern "C" void sub_820A1800(PPCContext& ctx, uint8_t* base);         // lock a vertex buffer
extern "C" void sub_820A1850(PPCContext& ctx, uint8_t* base);         // unlock it
float gw_sine(float x);

namespace {

constexpr u32 C = 0x82000000, D = 0x82180000, S = 0x821B0000;
constexpr u32 kFade = S + 18264, kBufferIndex = S + 18284, kBuffers = S + 18268, kStars = S + 18288;
constexpr u32 kStarCount = 6000, kStarSize = 20, kVertexSize = 24;

inline float lf(u32 a) {
  u32 v;
  memcpy(&v, gw_host(a), 4);
  v = __builtin_bswap32(v);
  float f;
  memcpy(&f, &v, 4);
  return f;
}

inline void sf(u32 a, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  memcpy(gw_host(a), &v, 4);
}

inline float ld(u32 a) {
  u64 v = gw_load64(a);
  double d;
  memcpy(&d, &v, 8);
  return (float)d;
}

inline float sel(float a, float b, float c) { return a >= 0.0f ? b : c; }  // fsel

float sine(PPCContext& ctx, uint8_t* base, float x) {
  if (__builtin_fabsf(x) < 2048.0f) return gw_sine(x);
  ctx.f1.f64 = x;
  __imp__sub_8212E9B0(ctx, base);
  return (float)ctx.f1.f64;
}

}  // namespace

extern "C" void sub_82075548(PPCContext& ctx, uint8_t* base) {
  GwPartTimer timer(kPartStars);
  const float one = lf(C + 1732);

  // The fade-in (clamped to 1 and kept) and the next of the three buffers.
  const float fade = sel(one - (lf(kFade) + lf(C + 4456)), lf(kFade) + lf(C + 4456), one);
  sf(kFade, fade);
  const u32 index = (gw_load32(kBufferIndex) + 1) % 3;
  gw_store32(kBufferIndex, index);

  ctx.f1.f64 = (double)(one / lf(D + 9708));
  const u64 exponent = gw_load64(C + 4648);
  memcpy(&ctx.f2.f64, &exponent, 8);
  sub_821303B8(ctx, base);
  const float scale = (float)ctx.f1.f64;

  const u32 buffer = gw_load32(kBuffers + index * 4);
  ctx.r3.u64 = buffer;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  sub_820A1800(ctx, base);
  u32 out = ctx.r3.u32;
  // The buffer's host mirror (src/vertex_mirror.cpp; made on first use): the
  // vertices in the NV2A's order, drawn in place by the renderer.
  u8* host = gw_vertex_mirror_add(out, kStarCount * kVertexSize);

  const float red = lf(C + 12288), green = lf(C + 12292), blue = lf(C + 5140);
  const float c = lf(C + 3400), d = lf(C + 1876), b = lf(C + 4528), a = lf(C + 4500);
  const float k = lf(D + 9880);
  for (u32 n = 0, star = kStars; n < kStarCount; ++n, star += kStarSize, out += kVertexSize) {
    const float brightness = lf(star + 8);
    const float step = brightness * a + b;
    const float base_level = brightness * d + b;
    float phase = lf(star + 12) - step;
    phase = sel(phase, phase, one);
    sf(star + 12, phase);
    const float s = sine(ctx, base, phase * k);
    const float level = (((brightness + one) * s) * s * c + base_level) * scale * fade;
    const float v[6] = {lf(star + 0), lf(star + 4), lf(star + 16), level * red, level * green, level * blue};
    if (host) {
      memcpy(host + n * kVertexSize, v, sizeof(v));
    } else {
      for (int w = 0; w < 6; ++w) sf(out + 4 * w, v[w]);
    }
  }

  ctx.r3.u64 = gw_load32(kBuffers + index * 4);
  sub_820A1850(ctx, base);
}
