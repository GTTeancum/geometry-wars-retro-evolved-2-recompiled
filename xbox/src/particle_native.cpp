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

// The particle store's own values: host order (see sub_82063100 below).
inline float hf(u32 a) {
  float f;
  memcpy(&f, gw_host(a), 4);
  return f;
}

inline float ld(u32 a) {  // a double constant, as float
  u64 v = gw_load64(a);
  double d;
  memcpy(&d, &v, 8);
  return (float)d;
}

// fsel: a >= 0 ? b : c (NaN: c), without a branch: a compare mask and
// and/andn/or. The particle data makes many of these unpredictable, and as
// branches the mispredictions cost about 15% of the particle update on the
// console.
inline float sel(float a, float b, float c) {
  float m = 0.0f;
  __asm__("cmpless %[a], %[m]\n\t"  // m = 0 <= a, all bits or none (false for NaN)
          "andps %[m], %[b]\n\t"
          "andnps %[c], %[m]\n\t"
          "orps %[b], %[m]"
          : [m] "+x"(m), [b] "+x"(b)
          : [a] "x"(a), [c] "x"(c));
  return m;
}
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

// The update's inputs that are the same for every particle of a frame: the
// game's constants, the play area, the time step, the trail's size, and the
// gravity wells and black holes. Read once per call of the particle job
// instead of once per particle (each a big-endian load through gw_host:
// about 30 a particle, plus the wells and holes, in the hottest loop).
struct ParticleConstants {
  static constexpr int kCopied = 16;  // wells and holes the layout has room for; more are read in place
  float one, zero, half, bounce, reverse;
  float max_x, max_y, dt;
  float bias, pull, push;
  float smoothing, cap, lo, hi, fade, trail_offset, glow_k, alpha_min, brightness_bias;
  float length, width, u0, size;
  int wells, holes;
  u32 system;  // particle system + 40: wells (x, y, z) from +0, holes from +192, strengths from +384
  bool drawn;  // gw_frame_drawn: the quads written
  float well[kCopied][3], hole[kCopied][3], strength[kCopied];
};

void particle_constants(u32 particle_system, ParticleConstants& k) {
  const u32 system = particle_system + 40;
  k.system = system;
  k.one = lf(C + 1732);       // f0 in the PowerPC code
  k.zero = lf(C + 1740);      // f1
  k.half = lf(C + 1828);      // f7
  k.bounce = ld(C + 10112);   // f23
  k.reverse = ld(C + 10856);  // velocity factor on hitting a wall
  k.max_x = lf(D + 9652);
  k.max_y = lf(D + 9656);
  k.dt = lf(kDeltaTime);
  k.bias = lf(C + 9080);
  k.pull = lf(C + 6660);
  k.push = lf(C + 4356);
  k.smoothing = lf(C + 1816);
  k.cap = lf(C + 3216);
  k.lo = ld(C + 3376);
  k.hi = ld(C + 3192);
  k.fade = lf(C + 10852);
  k.trail_offset = lf(C + 1788);
  k.glow_k = lf(C + 1824);
  k.alpha_min = lf(C + 6680);
  k.brightness_bias = lf(kBrightnessBias);
  k.length = lf(kTrailLength);
  k.width = lf(kTrailWidth);
  k.u0 = lf(C + 3208);
  k.size = lf(C + 3364);
  k.drawn = gw_frame_drawn();
  k.wells = (int)gw_load32(system + 448);
  k.holes = (int)gw_load32(system + 452);
  for (int n = 0; n < k.wells && n < ParticleConstants::kCopied; ++n)
    for (int c = 0; c < 3; ++c) k.well[n][c] = lf(system + (u32)n * 12 + (u32)c * 4);
  for (int n = 0; n < k.holes && n < ParticleConstants::kCopied; ++n) {
    for (int c = 0; c < 3; ++c) k.hole[n][c] = lf(system + 192 + (u32)n * 12 + (u32)c * 4);
    k.strength[n] = lf(system + 384 + (u32)n * 4);
  }
}

// A well's or hole's coordinate (c: 0 x, 1 y, 2 z), past the copied ones
// read from the guest as the original does.
inline float well_at(const ParticleConstants& k, int n, int c) {
  return n < ParticleConstants::kCopied ? k.well[n][c] : lf(k.system + (u32)n * 12 + (u32)c * 4);
}
inline float hole_at(const ParticleConstants& k, int n, int c) {
  return n < ParticleConstants::kCopied ? k.hole[n][c] : lf(k.system + 192 + (u32)n * 12 + (u32)c * 4);
}
inline float strength_at(const ParticleConstants& k, int n) {
  return n < ParticleConstants::kCopied ? k.strength[n] : lf(k.system + 384 + (u32)n * 4);
}

void particle_update(const ParticleConstants& k, u32 in, u32 out, u32 streak, u8* streak_host, u8* glow_host);

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
// vertices), for a single particle: the job (sub_82063A38 below) reads the
// constants and finds the mirrors once for all its particles.
void gw_particle(u32 particle_system, u32 in, u32 out, u32 streak, u32 glow) {
  ParticleConstants k;
  particle_constants(particle_system, k);
  particle_update(k, in, out, streak, gw_vertex_mirror(streak, 128), gw_vertex_mirror(glow, 112));
}

namespace {

// streak_host, glow_host: the quads' places in the buffers' host mirrors
// (src/vertex_mirror.cpp), or null: the streak then goes big-endian into the
// guest buffer at 'streak', the glow splat is left out.
void particle_update(const ParticleConstants& k, u32 in, u32 out, u32 streak, u8* streak_host, u8* glow_host) {
  // Everything read from the particle before anything is written.
  const float px = lf(in + 0), py = lf(in + 4), pz = lf(in + 8);
  const float vx = lf(in + 12), vy = lf(in + 16), vz = lf(in + 20);
  const float red = lf(in + 24), green = lf(in + 28), blue = lf(in + 32);
  const float brightness = lf(in + 36), speed_smoothed = lf(in + 40);

  const float one = k.one, zero = k.zero, half = k.half, bounce = k.bounce, reverse = k.reverse;

  // 1. Move, and reflect off the walls (+-max_x, +-max_y).
  const float max_x = k.max_x, max_y = k.max_y;
  const float dt = k.dt;
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
  const int wells = k.wells;
  if (wells > 0) {
    const float bias = k.bias, pull = k.pull;
    for (int n = 0; n < wells; ++n) {
      float tz = well_at(k, n, 2) - z, tx = well_at(k, n, 0) - x, ty = well_at(k, n, 1) - y;
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
  const int holes = k.holes;
  if (holes > 0) {
    const float push = k.push;
    for (int n = 0; n < holes; ++n) {
      const float ty = hole_at(k, n, 1) - y, tz = hole_at(k, n, 2) - z, tx = hole_at(k, n, 0) - x;
      const float strength = strength_at(k, n);
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
  const float smoothing = k.smoothing;
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
  const float cap = k.cap, lo = k.lo, hi = k.hi;
  const float faded = brightness * k.fade;
  const float limit = sel(cap - faded, faded, cap);
  const float trail = smoothed * smoothing;
  const float trail_clamped = sel(one - trail, trail, one);
  float t = (trail - k.trail_offset) * half;
  t = sel(-t, lo, t);
  t = sel(t - one, hi, t);
  const float glow_k = k.glow_k;
  const float intensity = limit * (t * t * t * glow_k + one) * (trail_clamped * trail_clamped);
  float alpha = sel(intensity - k.alpha_min, hi, bounce) * brightness - k.brightness_bias;
#ifdef GW_PARTICLE_STRESS
  // Test only: no particle fades out, so the game's pool fills (the worst
  // case for the vertex buffers, which follow the busiest frame).
  if (!(alpha > 0.25f)) alpha = 0.25f;
#endif
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
  const float length = k.length, width = k.width;
  const float across_x = length * dir_y, across_y = length * dir_x;
  const float along_x = width * dir_x, along_y = width * dir_y;
  const float cr = intensity * red, cg = intensity * green, cb = intensity * blue;
  // The quads only for frames that are rendered (gw_frame_drawn).
  if (!k.drawn) return;
  const float u0 = k.u0;
  if (u8* host = streak_host) {
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
    const float size = k.size;
    if (u8* host = glow_host) {
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

// Four particles at once: particle_update's operations, each on four
// particles in the four lanes of an SSE register (the same single-precision
// operations, so each lane's results are those of particle_update). The
// particles are in the store's host order (sub_82063100 below), turned into
// lanes with transposes, the results written back per particle. All four are read before any is written: the job's compaction
// moves a particle only to its own slot or an earlier one, so no particle's
// new slot is another's old one in the same batch.
struct Lane {
  u32 in, out, streak;
  u8* streak_host;
  u8* glow_host;
};
typedef __m128 V;
inline V s1(float f) { return _mm_set1_ps(f); }
inline V add(V a, V b) { return _mm_add_ps(a, b); }
inline V sub(V a, V b) { return _mm_sub_ps(a, b); }
inline V mul(V a, V b) { return _mm_mul_ps(a, b); }
inline V div(V a, V b) { return _mm_div_ps(a, b); }
inline V neg(V a) { return _mm_xor_ps(a, _mm_set1_ps(-0.0f)); }
inline V sel(V a, V b, V c) {  // a >= 0 ? b : c (NaN: c), lane by lane
  const V m = _mm_cmple_ps(_mm_setzero_ps(), a);
  return _mm_or_ps(_mm_and_ps(m, b), _mm_andnot_ps(m, c));
}
inline V rsq(V x) {
#ifdef GW_GRID_EXACT_RSQ
  alignas(16) float f[4];
  _mm_store_ps(f, x);
  for (float& v : f) v = rsq(v);
  return _mm_load_ps(f);
#else
  return _mm_rsqrt_ps(x);  // frsqrte, as rsq: the same estimate in every lane
#endif
}

void particle_update4(const ParticleConstants& k, const Lane* lane, u32 n) {
  // Lanes past n repeat the first particle; their results are dropped. Each
  // particle is three 16-byte loads (the twelfth float is the next
  // particle's, or the bucket's count: inside the bucket either way, unused).
  V r0[4], r1[4], r2[4];
  for (u32 l = 0; l < 4; ++l) {
    const float* in = (const float*)gw_host(lane[l < n ? l : 0].in);
    r0[l] = _mm_loadu_ps(in);
    r1[l] = _mm_loadu_ps(in + 4);
    r2[l] = _mm_loadu_ps(in + 8);
  }
  _MM_TRANSPOSE4_PS(r0[0], r0[1], r0[2], r0[3]);
  _MM_TRANSPOSE4_PS(r1[0], r1[1], r1[2], r1[3]);
  _MM_TRANSPOSE4_PS(r2[0], r2[1], r2[2], r2[3]);
  const V px = r0[0], py = r0[1], pz = r0[2], vx = r0[3];
  const V vy = r1[0], vz = r1[1], red = r1[2], green = r1[3];
  const V blue = r2[0], brightness = r2[1], speed_smoothed = r2[2];
  const V one = s1(k.one), zero = s1(k.zero), half = s1(k.half), bounce = s1(k.bounce), reverse = s1(k.reverse);
  const V unit = s1(1.0f);  // recip's 1

  // 1. Move, and reflect off the walls.
  const V max_x = s1(k.max_x), max_y = s1(k.max_y), dt = s1(k.dt);
  const V dx = mul(vx, dt), dy = mul(vy, dt);
  V dz = mul(vz, dt);
  const V nx = add(px, dx), ny = add(py, dy), z = add(pz, dz);
  const V left = add(max_x, nx), right = sub(max_x, nx);
  const V bottom = add(max_y, ny), top = sub(max_y, ny);
  const V factor_x = sel(left, sel(right, bounce, reverse), reverse);
  const V factor_y = sel(bottom, sel(top, bounce, reverse), reverse);
  V x = sel(left, nx, sub(neg(max_x), left));
  x = sel(right, x, add(max_x, right));
  V y = sel(bottom, ny, sub(neg(max_y), bottom));
  y = sel(top, y, add(max_y, top));
  const V nearest_x = sel(left, right, left), nearest_y = sel(bottom, top, bottom);
  const V inside = sel(nearest_x, nearest_y, nearest_x);
  V mx = sel(inside, dx, mul(factor_x, dx));
  V my = sel(inside, dy, mul(factor_y, dy));

  // 2. Gravity wells.
  if (k.wells > 0) {
    const V bias = s1(k.bias), pull = s1(k.pull);
    for (int w = 0; w < k.wells; ++w) {
      V tz = sub(s1(well_at(k, w, 2)), z), tx = sub(s1(well_at(k, w, 0)), x), ty = sub(s1(well_at(k, w, 1)), y);
      const V d2 = add(add(mul(tx, tx), mul(tz, tz)), mul(ty, ty));
      const V reach = sub(d2, one);
      V s = mul(div(unit, d2), pull);
      s = sel(sub(half, s), s, half);
      const V swirl_x = mul(sub(ty, tx), s), swirl_y = mul(sub(neg(tx), ty), s), swirl_z = mul(zero, s);
      ty = add(swirl_y, ty);
      tz = add(swirl_z, tz);
      tx = add(swirl_x, tx);
      const V r = rsq(add(add(add(mul(tz, tz), mul(ty, ty)), mul(tx, tx)), bias));
      const V ax = mul(mul(tx, r), s), ay = mul(mul(ty, r), s), az = mul(mul(tz, r), s);
      mx = sel(reach, add(ax, mx), mx);
      my = sel(reach, add(ay, my), my);
      dz = sel(reach, add(az, dz), dz);
    }
  }

  // 3. Black holes.
  if (k.holes > 0) {
    const V push = s1(k.push);
    for (int h = 0; h < k.holes; ++h) {
      const V ty = sub(s1(hole_at(k, h, 1)), y), tz = sub(s1(hole_at(k, h, 2)), z), tx = sub(s1(hole_at(k, h, 0)), x);
      const V strength = s1(strength_at(k, h));
      const V dist = _mm_sqrt_ps(add(add(mul(ty, ty), mul(tz, tz)), mul(tx, tx)));
      const V inv = div(one, dist);
      const V outside = sub(dist, one);
      V s = mul(div(unit, dist), push);
      s = sel(sub(half, s), s, half);
      const V new_z = sub(dz, mul(mul(inv, tz), s));
      const V ax = mul(mul(mul(inv, tx), s), strength), ay = mul(mul(mul(ty, inv), s), strength);
      dz = sel(outside, new_z, dz);
      mx = sel(outside, sub(mx, ax), mx);
      my = sel(outside, sub(my, ay), my);
    }
  }

  // 4. New state.
  const V speed = _mm_sqrt_ps(add(mul(mx, mx), add(mul(dz, dz), mul(my, my))));
  const V smoothing = s1(k.smoothing);
  const V smoothed = add(mul(sub(speed, speed_smoothed), smoothing), speed_smoothed);
  const V faded = mul(brightness, s1(k.fade));
  const V limit = sel(sub(s1(k.cap), faded), faded, s1(k.cap));
  const V trail = mul(smoothed, smoothing);
  const V trail_clamped = sel(sub(one, trail), trail, one);
  V t = mul(sub(trail, s1(k.trail_offset)), half);
  t = sel(neg(t), s1(k.lo), t);
  t = sel(sub(t, one), s1(k.hi), t);
  const V glow_k = s1(k.glow_k);
  const V intensity = mul(mul(limit, add(mul(mul(mul(t, t), t), glow_k), one)), mul(trail_clamped, trail_clamped));
  V alpha = sub(mul(sel(sub(intensity, s1(k.alpha_min)), s1(k.hi), bounce), brightness), s1(k.brightness_bias));
#ifdef GW_PARTICLE_STRESS
  {
    const V m = _mm_cmpgt_ps(alpha, s1(0.25f));
    alpha = _mm_or_ps(_mm_and_ps(m, alpha), _mm_andnot_ps(m, s1(0.25f)));
  }
#endif
  {
    // Back to particles: two 16-byte stores and three floats each (a
    // twelfth would overwrite the next particle).
    V s0 = x, s1 = y, s2 = z, s3 = mx, s4 = my, s5 = dz, s6 = red, s7 = green;
    V s8 = blue, s9 = alpha, s10 = smoothed, s11 = _mm_setzero_ps();
    _MM_TRANSPOSE4_PS(s0, s1, s2, s3);
    _MM_TRANSPOSE4_PS(s4, s5, s6, s7);
    _MM_TRANSPOSE4_PS(s8, s9, s10, s11);
    const V w0[4] = {s0, s1, s2, s3}, w1[4] = {s4, s5, s6, s7}, w2[4] = {s8, s9, s10, s11};
    for (u32 l = 0; l < n; ++l) {
      float* out = (float*)gw_host(lane[l].out);
      _mm_storeu_ps(out, w0[l]);
      _mm_storeu_ps(out + 4, w1[l]);
      alignas(16) float t[4];
      _mm_store_ps(t, w2[l]);
      out[8] = t[0];
      out[9] = t[1];
      out[10] = t[2];
    }
  }

  // The streak's direction (none for a particle that did not move).
  const V sx = sub(x, px), sy = sub(y, py);
  const V r = rsq(add(mul(sy, sy), mul(sx, sx)));
  V dir_x = mul(sx, r), dir_y = mul(sy, r);
  const V bad = _mm_or_ps(_mm_cmpneq_ps(sub(dir_x, dir_x), _mm_setzero_ps()),
                          _mm_cmpneq_ps(sub(dir_y, dir_y), _mm_setzero_ps()));
  dir_x = _mm_andnot_ps(bad, dir_x);
  dir_y = _mm_andnot_ps(bad, dir_y);
  const V length = s1(k.length), width = s1(k.width);
  const V across_x = mul(length, dir_y), across_y = mul(length, dir_x);
  const V along_x = mul(width, dir_x), along_y = mul(width, dir_y);
  const V cr = mul(intensity, red), cg = mul(intensity, green), cb = mul(intensity, blue);
  if (!k.drawn) return;
  const V u0 = s1(k.u0);

  // The streak: four vertices (x, y, z, u, r, g, b, v), turned from lanes
  // into each particle's.
  {
    V corner[4][8] = {
        {sub(sub(px, across_x), along_x), sub(add(across_y, py), along_y), pz, u0, cr, cg, cb, u0},
        {sub(add(across_x, px), along_x), sub(sub(py, across_y), along_y), pz, one, cr, cg, cb, u0},
        {add(add(along_x, across_x), x), add(sub(y, across_y), along_y), z, one, cr, cg, cb, one},
        {add(sub(x, across_x), along_x), add(add(along_y, across_y), y), z, u0, cr, cg, cb, one},
    };
    for (u32 v = 0; v < 4; ++v) {
      _MM_TRANSPOSE4_PS(corner[v][0], corner[v][1], corner[v][2], corner[v][3]);
      _MM_TRANSPOSE4_PS(corner[v][4], corner[v][5], corner[v][6], corner[v][7]);
    }
    for (u32 l = 0; l < n; ++l) {
      if (u8* host = lane[l].streak_host) {
        for (u32 v = 0; v < 4; ++v) {
          _mm_storeu_ps((float*)(host + v * 32), corner[v][l]);
          _mm_storeu_ps((float*)(host + v * 32 + 16), corner[v][4 + l]);
        }
      } else {
        for (u32 v = 0; v < 4; ++v) {
          alignas(16) float f[8];
          _mm_store_ps(f, corner[v][l]);
          _mm_store_ps(f + 4, corner[v][4 + l]);
          streak_vertex(lane[l].streak + v * 32, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
      }
    }
  }

  if (kWriteGlowQuad) {
    const V mid_x = mul(add(px, x), half), mid_y = mul(add(py, y), half);
    const V gr = mul(cr, glow_k), gg = mul(cg, glow_k), gb = mul(cb, glow_k);
    const V size = s1(k.size);
    V corner[4][8] = {
        {sub(mid_x, size), sub(mid_y, size), zero, zero, gr, gg, gb, zero},
        {add(mid_x, size), sub(mid_y, size), one, zero, gr, gg, gb, zero},
        {add(mid_x, size), add(mid_y, size), one, one, gr, gg, gb, zero},
        {sub(mid_x, size), add(mid_y, size), zero, one, gr, gg, gb, zero},
    };
    for (u32 v = 0; v < 4; ++v) {
      _MM_TRANSPOSE4_PS(corner[v][0], corner[v][1], corner[v][2], corner[v][3]);
      _MM_TRANSPOSE4_PS(corner[v][4], corner[v][5], corner[v][6], corner[v][7]);
    }
    for (u32 l = 0; l < n; ++l) {
      u8* host = lane[l].glow_host;
      if (!host) continue;  // past the mirror: left out, as particle_update
      for (u32 v = 0; v < 4; ++v) {
        alignas(16) float f[8];
        _mm_store_ps(f, corner[v][l]);
        _mm_store_ps(f + 4, corner[v][4 + l]);
        memcpy(host + v * 28, f, 28);
      }
    }
  }
}

}  // namespace

// The particle store, kept in host order in the game's own buckets.
//
// The particles' 44 bytes were big-endian like all guest data, and the job
// byte-swapped every value in and out of every particle every frame (with a
// crowd and explosions, up to 16,000 particles: most of the update's time on
// the Pentium III, which has no SSE byte shuffle). Only five functions touch
// a particle's values: the add (0x82063100), the bucket clear (0x82063018),
// the constructor at the system's creation (0x82062E10: the zero constant
// into the first six values, the same bytes either way: checked at the
// first add), the job (0x82063A38) and the update (0x82063448, called only by
// the job). The first three and the job are native here and keep the values
// in host order, in place, so no extra memory. The bucket counts (+5632)
// and index stay big-endian (recompiled code reads them: 0x82062EE0,
// 0x82062FC8). sub_82063448 keeps the guest's order for anything else that
// calls it (nothing does now).
namespace {
constexpr u32 kStoreBucket = 5636, kStoreCount = 5632, kStoreParticle = 44;
inline void hs(u32 a, float f) { memcpy(gw_host(a), &f, 4); }
}  // namespace

// Adds a particle (guest 0x82063100): r3 the particle system, r4 its position,
// r5 its velocity, r6 its colour (3 floats each, guest order). Into the
// current bucket (index entry +12) while it has room, else the next one from
// the index (biased by +0, as the original), else nowhere.
extern "C" void sub_82063100(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 system = ctx.r3.u32;
  const u32 last = gw_load32(system + 12), index = gw_load32(system + 8);
  u32 bucket = gw_load32(system + 4) + gw_load32(index + last * 4) * kStoreBucket;
  if ((int)gw_load32(bucket + kStoreCount) >= 128) {
    const u32 bias = gw_load32(system + 0), next = last + 1;
    if ((int)next >= (int)bias) return;
    const u32 entry = gw_load32(index + next * 4);
    gw_store32(system + 12, next);
    const u32 number = entry - bias;
    gw_store32(index + next * 4, number);
    bucket = gw_load32(system + 4) + number * kStoreBucket;
  }
  static bool checked;
  if (!checked) {
    checked = true;
    if (gw_load32(C + 1740) != 0) gw_log("particles: the constructor's zero is not 0.0 (%08X)\n", gw_load32(C + 1740));
  }
  const u32 count = gw_load32(bucket + kStoreCount);
  const u32 p = bucket + count * kStoreParticle;
  gw_store32(bucket + kStoreCount, count + 1);
  const u32 from[3] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  for (u32 v = 0; v < 3; ++v)
    for (u32 c = 0; c < 3; ++c) hs(p + v * 12 + c * 4, lf(from[v] + c * 4));
  hs(p + 36, lf(C + 1780));
  // Its speed: fmuls, fmadds, fmadds, fsqrts.
  const double vx = hf(p + 12), vy = hf(p + 16), vz = hf(p + 20);
  const float vy2 = (float)(vy * vy);
  const float s1 = (float)(vx * vx + vy2);
  const float s2 = (float)(vz * vz + s1);
  hs(p + 40, __builtin_sqrtf(s2));
}

// Clears a bucket (guest 0x82063018): r3 the bucket; the first six values of
// its 128 particles set to the zero constant, its count to 0.
extern "C" void sub_82063018(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 bucket = ctx.r3.u32;
  const float zero = lf(C + 1740);
  for (u32 k = 0; k < 128; ++k)
    for (u32 v = 0; v < 6; ++v) hs(bucket + k * kStoreParticle + v * 4, zero);
  gw_store32(bucket + kStoreCount, 0);
}

// Test hook (tools/native_check): every particle value of buckets buckets
// (count of them) byte-swapped, between the guest's order and the store's.
void gw_particle_store_swap(u32 buckets, u32 count) {
  for (u32 b = 0; b < count; ++b)
    for (u32 k = 0; k < 128; ++k)
      for (u32 v = 0; v < 11; ++v) {
        const u32 a = buckets + b * kStoreBucket + k * kStoreParticle + v * 4;
        gw_store32(a, __builtin_bswap32(gw_load32(a)));
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
// The most live particles kept (a job; the first takes them all): about
// 2.4 MB stays free in XEMU at the cap with four players.
constexpr u32 kMaxLive = 16000;

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
#ifdef GW_PARTICLE_STRESS
u32 gw_particles_most;  // the most live particles in one job (reported at an allocation failure)
#endif

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
  ParticleConstants constants;
  particle_constants(system, constants);
  // The buffers' host mirrors: particle n's quads at n x 128 and n x 112
  // bytes, the first main_room and glow_room particles in the mirror.
  u8* const main_host = gw_vertex_mirror(streak, 128);
  u8* const glow_host = gw_vertex_mirror(glow, 112);
  const u32 main_room = main_host ? gw_vertex_mirror_room(streak) / 128 : 0;
  const u32 glow_room = glow_host ? gw_vertex_mirror_room(glow) / 112 : 0;
  u32 live = 0, dest = 0, used = 0;
  // Particles are updated four at a time (particle_update4).
  Lane batch[4];
  u32 batched = 0;
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
      // The particles four ahead asked for now (two lines: 44 bytes each):
      // the Pentium III fetches nothing ahead itself, and each particle was a
      // wait for memory (a quarter of the update on the console). Prefetching
      // never faults, past the end included.
      _mm_prefetch((const char*)gw_host(p + 4 * kParticle), _MM_HINT_T0);
      _mm_prefetch((const char*)gw_host(p + 4 * kParticle + 32), _MM_HINT_T0);
      u32 bright;
      memcpy(&bright, gw_host(p + 36), 4);  // (the store's host order)
      if ((int)bright < 0) continue;
      // Past kMaxLive the particle is dropped as a dead one is (not moved on
      // into the compacted buckets): the vertex buffers follow the busiest
      // frame, and 64 MB ran out at about 25,400 (four players, the stress
      // build); the busiest real play seen had about 5,200.
      if (live >= kMaxLive) continue;
      if (used >= kPerBucket) {
        sw(dest + kCount, used);
        dest = bucket;
        used = 0;
      }
      batch[batched++] = {p, dest + used * kParticle, streak, live < main_room ? main_host + live * 128 : nullptr,
                          live < glow_room ? glow_host + live * 112 : nullptr};
      if (batched == 4) {
        particle_update4(constants, batch, 4);
        batched = 0;
      }
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
  if (batched) particle_update4(constants, batch, batched);
  if (dest) sw(dest + kCount, used);
  sw(kLive + job * 4, live);
#ifdef GW_PARTICLE_STRESS
  {
    static u32 calls;
    u32& most = gw_particles_most;
    if (live > most) most = live;
    if (job == 0 && ++calls % 300 == 0) {  // (about every 5 s)
      gw_log("particles: %u live (most %u); buckets in use %u\n", live, most, lw(system + 12) + 1);
    }
  }
#endif
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
