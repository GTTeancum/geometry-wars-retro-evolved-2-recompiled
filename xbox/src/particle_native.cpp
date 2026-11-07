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
// precision. Both quads go into the buffers' host mirrors in the NV2A's
// order when they fit there (src/vertex_mirror.cpp), else big-endian into
// the guest buffer.
#include "runtime.h"

#include <xmmintrin.h>

namespace {

constexpr u32 C = 0x82000000, D = 0x82180000;
constexpr u32 kDeltaTime = 0x821B0000 + 16032 + 32;
constexpr u32 kTrailLength = 0x821B0000 + 16120 + 32;
constexpr u32 kTrailWidth = 0x821B0000 + 16172 + 32;
constexpr u32 kBrightnessBias = 0x821A0000 - 11648;

// The glow quads are the light splats of the grid's colour field.
constexpr bool kWriteGlowQuad = true;

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
  GwPartTimer timer(kPartParticles);
  gw_particle(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

// The particle update itself (sub_82063448's arguments: the particle
// system, the particle, its slot after compaction, its streak and glow
// vertices).
void gw_particle(u32 particle_system, u32 in, u32 out, u32 streak, u32 glow) {
  const u32 system = particle_system + 40;

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
  float dir_x = sx * r, dir_y = sy * r;
  // A particle that did not move this frame (sx = sy = 0, as in the ship's
  // spawn bursts) has no direction: 0 x infinity, not a number, in every
  // corner of its streak. The 360's GPU drops such a quad; the NV2A drew it
  // as a huge polygon over the screen. Without a direction the streak has
  // no size (all four corners at the particle), which shows the same as
  // dropping it.
  if (!(dir_x - dir_x == 0.0f) || !(dir_y - dir_y == 0.0f)) dir_x = dir_y = 0.0f;
  // The streak runs from the old position (two corners) to the new one (two
  // corners). 'across' is the trail length turned a quarter (x <- y, y <- x),
  // 'along' the width along the motion, as the PowerPC code combines them.
  const float length = lf(kTrailLength), width = lf(kTrailWidth);
  const float across_x = length * dir_y, across_y = length * dir_x;
  const float along_x = width * dir_x, along_y = width * dir_y;
  const float cr = intensity * red, cg = intensity * green, cb = intensity * blue;
  // The quads only for frames that are rendered (gw_frame_drawn).
  if (!gw_frame_drawn()) return;
  const float u0 = lf(C + 3208);
  if (u8* host = gw_vertex_mirror(streak, 128)) {
    const float quad[4][8] = {
        {(px - across_x) - along_x, (across_y + py) - along_y, pz, u0, cr, cg, cb, u0},
        {(across_x + px) - along_x, (py - across_y) - along_y, pz, one, cr, cg, cb, u0},
        {(along_x + across_x) + x, (y - across_y) + along_y, z, one, cr, cg, cb, one},
        {(x - across_x) + along_x, (along_y + across_y) + y, z, u0, cr, cg, cb, one},
    };
    memcpy(host, quad, sizeof(quad));
  } else {
    streak_vertex(streak + 0, (px - across_x) - along_x, (across_y + py) - along_y, pz, u0, cr, cg, cb, u0);
    streak_vertex(streak + 32, (across_x + px) - along_x, (py - across_y) - along_y, pz, one, cr, cg, cb, u0);
    streak_vertex(streak + 64, (along_x + across_x) + x, (y - across_y) + along_y, z, one, cr, cg, cb, one);
    streak_vertex(streak + 96, (x - across_x) + along_x, (along_y + across_y) + y, z, u0, cr, cg, cb, one);
  }

  if (kWriteGlowQuad) {
    const float mid_x = (px + x) * half, mid_y = (py + y) * half;
    const float gr = cr * glow_k, gg = cg * glow_k, gb = cb * glow_k;
    const float size = lf(C + 3364);
    if (u8* host = gw_vertex_mirror(glow, 112)) {
      const float quad[4][7] = {
          {mid_x - size, mid_y - size, zero, zero, gr, gg, gb},
          {mid_x + size, mid_y - size, one, zero, gr, gg, gb},
          {mid_x + size, mid_y + size, one, one, gr, gg, gb},
          {mid_x - size, mid_y + size, zero, one, gr, gg, gb},
      };
      memcpy(host, quad, sizeof(quad));
    }
    // Past the mirror the splat is left out: only the colour field draws
    // these, from the mirror only (src/nv2a.cpp), and writing the guest buffer
    // would commit its memory (it ran out in big explosions at 720p).
  }
}

extern "C" void sub_820A1800(PPCContext& ctx, uint8_t* base);  // vertex buffer lock
extern "C" void sub_820A1850(PPCContext& ctx, uint8_t* base);  // vertex buffer unlock

namespace {
inline u32 lw(u32 a) { return gw_load32(a); }
inline void sw(u32 a, u32 v) { gw_store32(a, v); }

// The particle vertex buffers: tables of buffers at kMainBuffers and
// kGlowBuffers, (frame x 6 + job) each; the frame number at kFrame; each
// job's count of live particles at kLive + 4 x job.
constexpr u32 kGlowBuffers = 0x821B3E7C, kMainBuffers = 0x821B3E80, kFrame = 0x821B3E84, kLive = 0x821B3E88;
// A particle system: +0 bucket index bias, +4 buckets (5636 bytes: 128
// particles of 44 bytes, then the count at +5632), +8 the bucket index
// array, +12 the last index, +496 the next index (shared by the jobs).
constexpr u32 kBucket = 5636, kParticle = 44, kPerBucket = 128, kCount = 5632;

u32 lock(PPCContext& ctx, uint8_t* base, u32 buffer) {
  ctx.r3.u64 = buffer;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  sub_820A1800(ctx, base);
  return ctx.r3.u32;
}
}  // namespace

// A job's share of the particles (guest 0x82063A38; r3 the particle system,
// r4 the job's context, +4 its index). Takes buckets from the shared index
// (+496) until past the last one, moves each live particle (brightness at +36
// not negative) through gw_particle into the next free slot of the share's
// first bucket with room (compacting them), writing its vertices in turn;
// an emptied bucket gets count 0 and its index entry biased by +0. Natively
// for the per-particle calls and the reserved-store increments, which the
// recompiled code made the most expensive part of the particles; the same
// stores in the same order otherwise. The jobs run one after another on
// the main thread (sub_82063F08 below), so the index needs no atomics.
extern "C" void sub_82063A38(PPCContext& ctx, uint8_t* base) {
  GwPartTimer timer(kPartParticles);
  const u32 system = ctx.r3.u32, job = lw(ctx.r4.u32 + 4);
  ctx.r1.u64 = ctx.r1.u32 - 224;  // the original's frame, below which the calls put theirs
  // This frame's buffers: the set by drawn frame (gw_particle_set_buffer),
  // put in the game's tables where its renderer reads them.
  const u32 slot = (lw(kFrame) * 6 + job) * 4;
  u32 main_buffer = lw(lw(kMainBuffers) + slot), glow_buffer = lw(lw(kGlowBuffers) + slot);
  if (const u32 m = gw_particle_set_buffer(gw_drawn_index() & 1, job, false)) {
    main_buffer = m;
    glow_buffer = gw_particle_set_buffer(gw_drawn_index() & 1, job, true);
    sw(lw(kMainBuffers) + slot, main_buffer);
    sw(lw(kGlowBuffers) + slot, glow_buffer);
  }
  u32 streak = lock(ctx, base, main_buffer);
  u32 glow = lock(ctx, base, glow_buffer);
  u32 live = 0, dest = 0, used = 0;
  for (;;) {
    const u32 next = system + 496;
    const int index = (int)lw(next);
    sw(next, (u32)(index + 1));
    if (index > (int)lw(system + 12)) break;
    const u32 entry = lw(system + 8) + (u32)index * 4;
    const u32 number = lw(entry);
    const u32 bucket = lw(system + 4) + number * kBucket;
    if (!dest) {
      dest = bucket;
      used = 0;
    }
    for (int k = 0; k < (int)lw(bucket + kCount); ++k) {
      const u32 p = bucket + (u32)k * kParticle;
      if ((int)lw(p + 36) < 0) continue;
      if (used >= kPerBucket) {
        sw(dest + kCount, used);
        dest = bucket;
        used = 0;
      }
      gw_particle(system, p, dest + used * kParticle, streak, glow);
      ++used;
      streak += 128;
      glow += 112;
      ++live;
    }
    if (bucket != dest) {
      sw(bucket + kCount, 0);
      sw(entry, lw(system + 0) + number);
    }
  }
  if (dest) sw(dest + kCount, used);
  sw(kLive + job * 4, live);
  ctx.r3.u64 = glow_buffer;
  sub_820A1850(ctx, base);
  ctx.r3.u64 = main_buffer;
  sub_820A1850(ctx, base);
  ctx.r1.u64 = ctx.r1.u32 + 224;
}

// Heap sort of the particle bucket index (guest 0x82064110 with its
// sift-down 0x82064060): r3 points at the array's address, r4 the count; the
// entries are signed 32-bit and sorted ascending. Equal entries are equal
// values, so any correct sort gives the same array: natively a heap sort
// on a host copy (up to kMaxSort entries; more: the original).
extern "C" void __imp__sub_82064110(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82064110(PPCContext& ctx, uint8_t* base) {
  constexpr int kMaxSort = 8192;
  const int n = (int)ctx.r4.u32;
  if (n > kMaxSort) {
    __imp__sub_82064110(ctx, base);
    return;
  }
  if (n <= 1) return;
  static int values[kMaxSort];
  const u32 array = lw(ctx.r3.u32);
  for (int i = 0; i < n; ++i) values[i] = (int)lw(array + (u32)i * 4);
  // Heap sort, as the original, on host integers.
  auto sift = [&](int root, int end) {
    for (int child; (child = 2 * root + 1) < end; root = child) {
      if (child + 1 < end && values[child] < values[child + 1]) ++child;
      if (!(values[root] < values[child])) return;
      const int t = values[root];
      values[root] = values[child];
      values[child] = t;
    }
  };
  for (int i = n / 2 - 1; i >= 0; --i) sift(i, n);
  for (int end = n - 1; end > 0; --end) {
    const int t = values[0];
    values[0] = values[end];
    values[end] = t;
    sift(0, end);
  }
  for (int i = 0; i < n; ++i) sw(array + (u32)i * 4, (u32)values[i]);
}
