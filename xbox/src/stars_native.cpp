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

#include <xmmintrin.h>

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

// SSE masks and their and/or as instructions: the compiler types them as
// integer vectors, which the Pentium III's SSE does not have (it splits them
// into scalar code).
inline __m128 and4(__m128 a, __m128 b) {
  __asm__("andps %1, %0" : "+x"(a) : "x"(b));
  return a;
}
inline __m128 xor4(__m128 a, __m128 b) {
  __asm__("xorps %1, %0" : "+x"(a) : "x"(b));
  return a;
}
inline __m128 less4(__m128 a, __m128 b) {  // a < b (false for NaN)
  __asm__("cmpltps %1, %0" : "+x"(a) : "x"(b));
  return a;
}
inline __m128 unequal4(__m128 a, __m128 b) {  // a != b (true for NaN)
  __asm__("cmpneqps %1, %0" : "+x"(a) : "x"(b));
  return a;
}
// floor(v) for 0 <= v < 2^23, which for those is (float)(int)v: rounded to a
// whole number by adding and taking away 2^23, one less where that rounded up.
inline __m128 whole4(__m128 v) {
  const __m128 big = _mm_set1_ps(8388608.0f);
  const __m128 t = _mm_sub_ps(_mm_add_ps(v, big), big);
  return _mm_sub_ps(t, and4(less4(v, t), _mm_set1_ps(1.0f)));
}

// gw_sine (src/math_native.cpp) in four lanes, its operations in the same
// order, so each lane's result has the same bits; a lane outside +-2048 (or
// not a number) gets sine()'s answer, the original's.
__m128 sine4(PPCContext& ctx, uint8_t* base, __m128 x) {
  u32 bits = 0x80000000u;
  float sign_float;
  memcpy(&sign_float, &bits, 4);
  const __m128 sign = _mm_set1_ps(sign_float);
  bits = 0x7FFFFFFFu;
  float abs_float;
  memcpy(&abs_float, &bits, 4);
  const __m128 ax = and4(x, _mm_set1_ps(abs_float));
  const int in_range = _mm_movemask_ps(less4(ax, _mm_set1_ps(2048.0f)));
  const __m128 k = whole4(_mm_add_ps(_mm_mul_ps(ax, _mm_set1_ps(0.318309886f)), _mm_set1_ps(0.5f)));
  __m128 r = _mm_sub_ps(ax, _mm_mul_ps(k, _mm_set1_ps(3.140625f)));
  r = _mm_sub_ps(r, _mm_mul_ps(k, _mm_set1_ps(9.67653589793e-4f)));
  const __m128 r2 = _mm_mul_ps(r, r);
  __m128 p = _mm_set1_ps(-2.50521084e-8f);
  p = _mm_add_ps(_mm_mul_ps(p, r2), _mm_set1_ps(2.75573192e-6f));
  p = _mm_sub_ps(_mm_mul_ps(p, r2), _mm_set1_ps(1.98412698e-4f));
  p = _mm_add_ps(_mm_mul_ps(p, r2), _mm_set1_ps(8.33333333e-3f));
  p = _mm_sub_ps(_mm_mul_ps(p, r2), _mm_set1_ps(1.66666667e-1f));
  __m128 s = _mm_add_ps(r, _mm_mul_ps(_mm_mul_ps(r, r2), p));
  const __m128 h = _mm_mul_ps(k, _mm_set1_ps(0.5f));
  s = xor4(s, and4(unequal4(h, whole4(h)), sign));        // k odd: -s
  s = xor4(s, and4(less4(x, _mm_setzero_ps()), sign));  // x < 0: -s
  if (in_range != 15) {
    alignas(16) float in[4], out[4];
    _mm_store_ps(in, x);
    _mm_store_ps(out, s);
    for (int l = 0; l < 4; ++l)
      if (!(in_range >> l & 1)) out[l] = sine(ctx, base, in[l]);
    s = _mm_load_ps(out);
  }
  return s;
}

}  // namespace

extern "C" void sub_82075548(PPCContext& ctx, uint8_t* base) {
  GwPartTimer timer(kPartStars);
  const float one = lf(C + 1732);

  // The fade-in (clamped to 1 and kept) and the next of the three buffers.
  const float fade = sel(one - (lf(kFade) + lf(C + 4456)), lf(kFade) + lf(C + 4456), one);
  sf(kFade, fade);

  // A frame that is not rendered (gw_frame_drawn) only moves the stars'
  // twinkle on; their vertices, the buffer and the sines wait for a rendered
  // frame.
  if (!gw_frame_drawn()) {
    const float a = lf(C + 4500), b = lf(C + 4528);
    for (u32 n = 0, star = kStars; n < kStarCount; ++n, star += kStarSize) {
      const float phase = lf(star + 12) - (lf(star + 8) * a + b);
      sf(star + 12, sel(phase, phase, one));
    }
    return;
  }
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
  // Four stars at a time (6000 is a multiple of four), each lane the
  // operations of one star in the same order as one at a time, so the same
  // results; the sine (most of a star's time) as sine4.
  static_assert(kStarCount % 4 == 0);
  const __m128 a4 = _mm_set1_ps(a), b4 = _mm_set1_ps(b), c4 = _mm_set1_ps(c), d4 = _mm_set1_ps(d);
  const __m128 one4 = _mm_set1_ps(one), k4 = _mm_set1_ps(k), scale4 = _mm_set1_ps(scale), fade4 = _mm_set1_ps(fade);
  for (u32 n = 0, star = kStars; n < kStarCount; n += 4, star += 4 * kStarSize, out += 4 * kVertexSize) {
    const __m128 brightness = _mm_setr_ps(lf(star + 8), lf(star + 8 + kStarSize), lf(star + 8 + 2 * kStarSize),
                                          lf(star + 8 + 3 * kStarSize));
    const __m128 old_phase = _mm_setr_ps(lf(star + 12), lf(star + 12 + kStarSize), lf(star + 12 + 2 * kStarSize),
                                         lf(star + 12 + 3 * kStarSize));
    const __m128 step = _mm_add_ps(_mm_mul_ps(brightness, a4), b4);
    const __m128 base_level = _mm_add_ps(_mm_mul_ps(brightness, d4), b4);
    // The new phase (sel(phase, phase, one): one where it went below 0), stored
    // big-endian per star.
    alignas(16) float phases[4];
    _mm_store_ps(phases, _mm_sub_ps(old_phase, step));
    for (int l = 0; l < 4; ++l) {
      phases[l] = sel(phases[l], phases[l], one);
      sf(star + (u32)l * kStarSize + 12, phases[l]);
    }
    const __m128 phase = _mm_load_ps(phases);
    const __m128 s = sine4(ctx, base, _mm_mul_ps(phase, k4));
    const __m128 level = _mm_mul_ps(
        _mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_mul_ps(_mm_mul_ps(_mm_add_ps(brightness, one4), s), s), c4), base_level),
                   scale4),
        fade4);
    alignas(16) float colour[3][4];
    _mm_store_ps(colour[0], _mm_mul_ps(level, _mm_set1_ps(red)));
    _mm_store_ps(colour[1], _mm_mul_ps(level, _mm_set1_ps(green)));
    _mm_store_ps(colour[2], _mm_mul_ps(level, _mm_set1_ps(blue)));
    for (int l = 0; l < 4; ++l) {
      const u32 at = star + (u32)l * kStarSize;
      const float v[6] = {lf(at + 0), lf(at + 4), lf(at + 16), colour[0][l], colour[1][l], colour[2][l]};
      if (host) {
        memcpy(host + (n + (u32)l) * kVertexSize, v, sizeof(v));
      } else {
        for (int w = 0; w < 6; ++w) sf(out + (u32)l * kVertexSize + 4 * w, v[w]);
      }
    }
  }

  ctx.r3.u64 = gw_load32(kBuffers + index * 4);
  sub_820A1850(ctx, base);
}
