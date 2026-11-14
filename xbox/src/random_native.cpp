// Native replacements for the game's random number generator: a Mersenne
// Twister (MT19937) whose state the game keeps in guest memory (624 words,
// big-endian, then +2492 the last word written by the regeneration and
// +2496 the index), seeded by guest 0x82030010. Enemies, separation's random
// pushes and the particles' spawns call it thousands of times a frame in a
// crowd, through recompiled code that kept every value in memory. The state
// (words 0-623, then +2496 the index) stays where the game keeps it, in its
// byte order, so the seeding and anything else that reads it are unchanged;
// the results are the original's (checked by tools/random_check.cmd).
//   0x82030058  r3 the state -> r3 a 32-bit integer
//   0x82030220  r3 the state, f1 a scale -> f1 in [-1, 1) x scale
//   0x82030430  r3 the state, f1 a scale -> f1 in [0, 2) x scale (as 0x82030220
//               without its offset)
//   0x8203B8B0  r3 a 2D vector out, r4 the state, f1 a scale -> r3; y then x
//               from 0x82030220's formula
#include "runtime.h"

namespace {

constexpr u32 C = 0x82000000;
constexpr u32 kN = 624, kM = 397;
constexpr u32 kLast = 2492, kIndex = 2496;  // bytes into the state: word 623, the index

inline float lfs(u32 a) {
  const u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline void stfs(u32 a, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  gw_store32(a, v);
}

// The state's words on the host while regenerating (624 big-endian loads
// and stores either way; done once every 624 numbers).
u32 next_u32(u32 state) {
  u32 index = gw_load32(state + kIndex);
  if ((int)index >= (int)kN) {
    const u32 mag[2] = {gw_load32(C + 1724), gw_load32(C + 1728)};  // {0, 0x9908B0DF}
    static u32 mt[kN];
    for (u32 k = 0; k < kN; ++k) mt[k] = gw_load32(state + k * 4);
    for (u32 k = 0; k < kN - kM; ++k) {  // 227
      const u32 y = (mt[k + 1] & 0x7FFFFFFF) | (mt[k] & 0x80000000);
      mt[k] = mt[k + kM] ^ (y >> 1) ^ mag[y & 1];
    }
    for (u32 k = kN - kM; k < kN - 1; ++k) {  // 396
      const u32 y = (mt[k + 1] & 0x7FFFFFFF) | (mt[k] & 0x80000000);
      mt[k] = mt[k + kM - kN] ^ (y >> 1) ^ mag[y & 1];
    }
    // The last word (623, at +2492), paired with word 0 as regenerated above
    // and from word 396 as regenerated above, as the original does.
    const u32 y = (mt[0] & 0x7FFFFFFF) | (mt[kN - 1] & 0x80000000);
    const u32 w = mt[kM - 1] ^ (y >> 1) ^ mag[y & 1];
    for (u32 k = 0; k < kN - 1; ++k) gw_store32(state + k * 4, mt[k]);
    gw_store32(state + kLast, w);
    index = 0;
  }
  u32 y = gw_load32(state + index * 4);
  gw_store32(state + kIndex, index + 1);
  y ^= y >> 11;
  y ^= (y << 7) & 0x9D2C5680;
  y ^= (y << 15) & 0xEFC60000;
  y ^= y >> 18;
  return y;
}

// 0x82030220's arithmetic: fcfid, frsp, fmsubs with the constants, fmuls.
inline double signed_unit(u32 r, double scale) {
  const double f12 = (double)(float)(double)r;
  const double k = lfs(C + 1736), one = lfs(C + 1732);
  const double f0 = (double)(float)(f12 * k + -one);
  return (double)(float)(f0 * scale);
}

}  // namespace

#ifndef GW_RANDOM_CHECK
extern "C" void sub_82030058(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = next_u32(ctx.r3.u32);
}

extern "C" void sub_82030220(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.f1.f64 = signed_unit(next_u32(ctx.r3.u32), ctx.f1.f64);
}

extern "C" void sub_82030430(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const double scale = ctx.f1.f64;
  const double f13 = (double)(float)(double)next_u32(ctx.r3.u32);
  const double f0 = (double)(float)(f13 * (double)lfs(C + 1764));
  ctx.f1.f64 = (double)(float)(f0 * scale);
}

extern "C" void sub_8203B8B0(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 out = ctx.r3.u32, state = ctx.r4.u32;
  const double scale = ctx.f1.f64;
  const float y = (float)signed_unit(next_u32(state), scale);
  stfs(out + 4, y);
  const float x = (float)signed_unit(next_u32(state), scale);
  stfs(out + 0, x);
}
#endif
