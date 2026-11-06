// Native replacement for the game's per-particle update (guest 0x82063448),
// called by the particle job for every live particle each frame.
//
// One particle (44 bytes, big-endian floats):
//   +0 position x, y, z   +12 velocity x, y, z   +24 colour r, g, b
//   +36 brightness        +40 smoothed speed (trail length)
// The update:
//   1. moves the particle by velocity * dt and reflects it off the walls of
//      the play area (velocity scaled by a bounce factor on the wall's axis);
//   2. pulls it towards each gravity well inside the well's reach;
//   3. pushes it away from each black hole;
//   4. writes the new state, and the particle's streak as a quad of four
//      32-byte vertices (x, y, z, u, r, g, b, v) for the main picture, oriented
//      along its motion, plus a 28-byte-vertex quad for the glow pass.
//
// The maths follows the PowerPC code operation for operation, in single
// precision. The glow quad is not written while src/nv2a.cpp does not draw
// the glow pass (kWriteGlowQuad).
#include "runtime.h"

#include <xmmintrin.h>

namespace {

constexpr u32 C = 0x82000000, D = 0x82180000;
constexpr u32 kDeltaTime = 0x821B0000 + 16032 + 32;
constexpr u32 kTrailLength = 0x821B0000 + 16120 + 32;
constexpr u32 kTrailWidth = 0x821B0000 + 16172 + 32;
constexpr u32 kBrightnessBias = 0x821A0000 - 11648;

#ifdef GW_PARTICLE_GLOW
constexpr bool kWriteGlowQuad = true;
#else
constexpr bool kWriteGlowQuad = false;  // the glow pass is not drawn yet
#endif

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

inline float ld(u32 a) {  // a double constant, as float
  u64 v = gw_load64(a);
  double d;
  memcpy(&d, &v, 8);
  return (float)d;
}

inline float sel(float a, float b, float c) { return a >= 0.0f ? b : c; }  // fsel
inline float recip(float x) { return 1.0f / x; }                         // fres
#ifdef GW_GRID_EXACT_RSQ
inline float rsq(float x) { return (float)(1.0 / __builtin_sqrt((double)x)); }
#else
inline float rsq(float x) { return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x))); }  // frsqrte
#endif

// One 32-byte vertex of the main-picture quad.
inline void streak_vertex(u32 out, float x, float y, float z, float u, float r, float g, float b, float v) {
  sf(out + 0, x);
  sf(out + 4, y);
  sf(out + 8, z);
  sf(out + 12, u);
  sf(out + 16, r);
  sf(out + 20, g);
  sf(out + 24, b);
  sf(out + 28, v);
}

// One 28-byte vertex of the glow quad.
inline void glow_vertex(u32 out, float x, float y, float z, float u, float r, float g, float b) {
  sf(out + 0, x);
  sf(out + 4, y);
  sf(out + 8, z);
  sf(out + 12, u);
  sf(out + 16, r);
  sf(out + 20, g);
  sf(out + 24, b);
}

}  // namespace

// r3 particle system, r4 particle (read), r5 particle (written; may be the
// same one), r6 main-picture quad, r7 glow quad.
extern "C" void sub_82063448(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 system = ctx.r3.u32 + 40, in = ctx.r4.u32, out = ctx.r5.u32;
  const u32 streak = ctx.r6.u32, glow = ctx.r7.u32;

  // Everything read from the particle before anything is written.
  const float px = lf(in + 0), py = lf(in + 4), pz = lf(in + 8);
  const float vx = lf(in + 12), vy = lf(in + 16), vz = lf(in + 20);
  const float red = lf(in + 24), green = lf(in + 28), blue = lf(in + 32);
  const float brightness = lf(in + 36), speed_smoothed = lf(in + 40);

  const float one = lf(C + 1732);       // f0 in the PowerPC code
  const float zero = lf(C + 1740);      // f1
  const float half = lf(C + 1828);      // f7
  const float bounce = ld(C + 10112);   // f23
  const float reverse = ld(C + 10856);  // velocity factor on hitting a wall

  // 1. Move, and reflect off the walls (+-max_x, +-max_y).
  const float max_x = lf(D + 9652), max_y = lf(D + 9656);
  const float dt = lf(kDeltaTime);
  const float dx = vx * dt, dy = vy * dt;
  float dz = vz * dt;
  const float nx = px + dx, ny = py + dy, z = pz + dz;
  const float left = max_x + nx, right = max_x - nx;  // >= 0 while inside
  const float bottom = max_y + ny, top = max_y - ny;
  const float factor_x = sel(left, sel(right, bounce, reverse), reverse);
  const float factor_y = sel(bottom, sel(top, bounce, reverse), reverse);
  float x = sel(left, nx, -max_x - left);
  x = sel(right, x, max_x + right);
  float y = sel(bottom, ny, -max_y - bottom);
  y = sel(top, y, max_y + top);
  const float nearest_x = sel(left, right, left), nearest_y = sel(bottom, top, bottom);
  const float inside = sel(nearest_x, nearest_y, nearest_x);
  float mx = sel(inside, dx, factor_x * dx);  // the step, now the velocity carried on
  float my = sel(inside, dy, factor_y * dy);

  // 2. Gravity wells (x, y, z; 12 bytes each from +0).
  const int wells = (int)gw_load32(system + 448);
  if (wells > 0) {
    const float bias = lf(C + 9080), pull = lf(C + 6660);
    for (int n = 0; n < wells; ++n) {
      const u32 w = system + (u32)n * 12;
      float tz = lf(w + 8) - z, tx = lf(w + 0) - x, ty = lf(w + 4) - y;
      const float d2 = tx * tx + tz * tz + ty * ty;  // fmadds order: z, then x, then y
      const float reach = d2 - one;
      float s = recip(d2) * pull;
      s = sel(half - s, s, half);
      // A swirl: the pull is bent sideways (x - y, -x - y) before normalising.
      const float swirl_x = (ty - tx) * s, swirl_y = (-tx - ty) * s, swirl_z = zero * s;
      ty = swirl_y + ty;
      tz = swirl_z + tz;
      tx = swirl_x + tx;
      const float r = rsq((tz * tz + ty * ty) + tx * tx + bias);
      const float ax = tx * r * s, ay = ty * r * s, az = tz * r * s;
      mx = sel(reach, ax + mx, mx);
      my = sel(reach, ay + my, my);
      dz = sel(reach, az + dz, dz);
    }
  }

  // 3. Black holes (x, y, z from +192; strength from +384).
  const int holes = (int)gw_load32(system + 452);
  if (holes > 0) {
    const float push = lf(C + 4356);
    for (int n = 0; n < holes; ++n) {
      const u32 h = system + 192 + (u32)n * 12;
      const float ty = lf(h + 4) - y, tz = lf(h + 8) - z, tx = lf(h + 0) - x;
      const float strength = lf(system + 384 + (u32)n * 4);
      const float dist = __builtin_sqrtf((ty * ty + tz * tz) + tx * tx);
      const float inv = one / dist;
      const float outside = dist - one;
      float s = recip(dist) * push;
      s = sel(half - s, s, half);
      const float new_z = dz - (inv * tz) * s;  // not scaled by strength, as in the original
      const float ax = (inv * tx) * s * strength, ay = (ty * inv) * s * strength;
      dz = sel(outside, new_z, dz);
      mx = sel(outside, mx - ax, mx);
      my = sel(outside, my - ay, my);
    }
  }

  // 4. New state.
  const float speed = __builtin_sqrtf(mx * mx + (dz * dz + my * my));
  const float smoothing = lf(C + 1816);
  const float smoothed = (speed - speed_smoothed) * smoothing + speed_smoothed;
  sf(out + 0, x);
  sf(out + 4, y);
  sf(out + 8, z);
  sf(out + 12, mx);
  sf(out + 16, my);
  sf(out + 20, dz);
  sf(out + 24, red);
  sf(out + 28, green);
  sf(out + 32, blue);
  sf(out + 40, smoothed);

  // Brightness: grows with the trail, fades with the particle's own value.
  const float cap = lf(C + 3216), lo = ld(C + 3376), hi = ld(C + 3192);
  const float faded = brightness * lf(C + 10852);
  const float limit = sel(cap - faded, faded, cap);
  const float trail = smoothed * smoothing;
  const float trail_clamped = sel(one - trail, trail, one);
  float t = (trail - lf(C + 1788)) * half;
  t = sel(-t, lo, t);
  t = sel(t - one, hi, t);
  const float glow_k = lf(C + 1824);
  const float intensity = limit * (t * t * t * glow_k + one) * (trail_clamped * trail_clamped);
  const float alpha = sel(intensity - lf(C + 6680), hi, bounce) * brightness - lf(kBrightnessBias);
  sf(out + 36, alpha);

  // Direction of travel this frame (normalised), for the streak.
  const float sx = x - px, sy = y - py;
  const float r = rsq(sy * sy + sx * sx);
  const float dir_x = sx * r, dir_y = sy * r;
  // The streak runs from the old position (two corners) to the new one (two
  // corners). 'across' is the trail length turned a quarter (x <- y, y <- x),
  // 'along' the width along the motion, as the PowerPC code combines them.
  const float length = lf(kTrailLength), width = lf(kTrailWidth);
  const float across_x = length * dir_y, across_y = length * dir_x;
  const float along_x = width * dir_x, along_y = width * dir_y;
  const float cr = intensity * red, cg = intensity * green, cb = intensity * blue;
  const float u0 = lf(C + 3208);
  streak_vertex(streak + 0, (px - across_x) - along_x, (across_y + py) - along_y, pz, u0, cr, cg, cb, u0);
  streak_vertex(streak + 32, (across_x + px) - along_x, (py - across_y) - along_y, pz, one, cr, cg, cb, u0);
  streak_vertex(streak + 64, (along_x + across_x) + x, (y - across_y) + along_y, z, one, cr, cg, cb, one);
  streak_vertex(streak + 96, (x - across_x) + along_x, (along_y + across_y) + y, z, u0, cr, cg, cb, one);

  if (kWriteGlowQuad) {
    const float mid_x = (px + x) * half, mid_y = (py + y) * half;
    const float gr = cr * glow_k, gg = cg * glow_k, gb = cb * glow_k;
    const float size = lf(C + 3364);
    glow_vertex(glow + 0, mid_x - size, mid_y - size, zero, zero, gr, gg, gb);
    glow_vertex(glow + 28, mid_x + size, mid_y - size, one, zero, gr, gg, gb);
    glow_vertex(glow + 56, mid_x + size, mid_y + size, one, one, gr, gg, gb);
    glow_vertex(glow + 84, mid_x - size, mid_y + size, zero, one, gr, gg, gb);
  }
}
