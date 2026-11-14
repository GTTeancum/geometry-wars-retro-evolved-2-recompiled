// Native rewrites of the game's per-enemy work, each checked against the
// recompiled original by tools/native_check.cmd.
//
// Separation (guest 0x82046248): an enemy and every other enemy of its kind
// that overlaps it are pushed apart. Called for each enemy every update, it
// was the main thread's largest recompiled cost in a crowded Evolved game.
//   r3  the enemy: position +4/+8, velocity +12/+16, radius +32, kind +68
//   f1  how far around it to look; f2 the push's strength
// The game's spatial hash (a 32 x 32 grid of 32-unit cells, wrapping; its
// table's address at 0x8219E850, 16 bytes a cell, a circular list of
// {entity, next} entries from +4) is walked over the cells the look-around
// covers, row by row. Two enemies at almost the same spot get a random push
// each (the game's own sub_8203B8B0, its generator at 0x821A1E28). Same
// single-precision operations in the same order as the original, so the
// results are bit-identical.
#include "runtime.h"

#include <xmmintrin.h>

extern "C" void sub_8203B8B0(PPCContext& ctx, uint8_t* base);  // random vector (guest)

namespace {

constexpr u32 kHashTable = 0x8219E850;
constexpr u32 kRandom = 0x821A1E28;
constexpr u32 kHalf = 0x82000000 + 1828, kOne = 0x82000000 + 1732, kTiny = 0x82000000 + 1812;

inline float ldf(u32 a) {
  const u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline void stf(u32 a, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  gw_store32(a, v);
}

// fctiwz, then >> 5 and the low 5 bits: the cell of a coordinate.
inline u32 cell(float v) {
  i32 i;
  if (v != v) i = (i32)0x80000000;                    // NaN
  else if (v >= 2147483648.0f) i = 0x7FFFFFFF;         // saturates as the PowerPC does
  else if (v < -2147483648.0f) i = (i32)0x80000000;
  else i = (i32)v;                                     // toward zero
  return (u32)(i >> 5) & 31;
}

}  // namespace

extern "C" void sub_82046248(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const float reach = (float)ctx.f1.f64, strength = (float)ctx.f2.f64;
  const u32 kind = gw_load32(self + 68);
  const float sx = ldf(self + 4), sy = ldf(self + 8);
  const u32 x0 = cell(sx - reach), y0 = cell(sy - reach);
  const u32 x1 = cell(sx + reach), y1 = cell(sy + reach);
  const float half = ldf(kHalf), one = ldf(kOne), tiny = ldf(kTiny);
  const u32 table = gw_load32(kHashTable);
  u32 cx = (x0 - 1) & 31, cy = y0;
  u32 bucket = 0, entry = 0;
  // The next cell with anything in it (bucket, its first entry), or entry 0.
  auto next_cell = [&]() {
    for (;;) {
      if (cx != x1) {
        cx = (cx + 1) & 31;
      } else {
        if (cy == y1) {
          entry = 0;
          return;
        }
        cy = (cy + 1) & 31;
        cx = x0;
      }
      bucket = table + (((cy << 5) | cx) << 4);
      entry = gw_load32(bucket + 4);
      if (entry != bucket) return;
    }
  };
  next_cell();
  // Only velocities change here: the enemy's own position and size are read
  // once. Each step also asks for the next entry, and for the entity after
  // it, ahead of time (a crowd's lists are long and each step waited on
  // memory); the arithmetic and its order are unchanged.
  const float self_radius = ldf(self + 32);
  while (entry) {
    const u32 ahead = gw_load32(entry + 4);
    if (ahead != bucket) {
      _mm_prefetch((const char*)gw_host(ahead), _MM_HINT_T0);
      const u32 ahead_entity = gw_load32(ahead);  // usually fetched by the step before
      if (ahead_entity) {
        _mm_prefetch((const char*)gw_host(ahead_entity), _MM_HINT_T0);
        _mm_prefetch((const char*)gw_host(ahead_entity + 32), _MM_HINT_T0);
        _mm_prefetch((const char*)gw_host(ahead_entity + 64), _MM_HINT_T0);
      }
    }
    const u32 other = gw_load32(entry);
    if (!other) break;
    if (gw_load32(other + 68) == kind && other != self) {
      const float sum = ldf(other + 32) + self_radius;
      const float dx = ldf(other + 4) - sx;
      const float dy = ldf(other + 8) - sy;
      const float reach2 = sum * sum;
      const float d2 = dy * dy + dx * dx;
      if (d2 < reach2) {
        if (d2 < tiny) {
          // On top of each other: a random push each, from the game's generator.
          const u32 sp = ctx.r1.u32;
          ctx.r1.u64 = sp - 240;
          for (int k = 0; k < 2; ++k) {
            const u32 who = k == 0 ? self : other;
            ctx.r3.u64 = ctx.r1.u32 + 80 + k * 8;
            ctx.r4.u64 = kRandom;
            ctx.f1.f64 = one;
            sub_8203B8B0(ctx, base);
            const u32 v = ctx.r3.u32;
            stf(who + 12, ldf(who + 12) + ldf(v));
            stf(who + 16, ldf(v + 4) + ldf(who + 16));
          }
          ctx.r1.u64 = sp;
        } else {
          const float dist = __builtin_sqrtf(d2);
          const float inv = one / dist;
          const float overlap = (sum - dist) * strength;
          const float nx = inv * dx, ny = inv * dy;
          const float px = nx * overlap, py = ny * overlap;
          stf(self + 12, ldf(self + 12) - px * half);
          stf(self + 16, ldf(self + 16) - py * half);
          stf(other + 12, ldf(other + 12) + px * half);
          stf(other + 16, ldf(other + 16) + py * half);
        }
      }
    }
    entry = gw_load32(entry + 4);
    if (entry == bucket) next_cell();
  }
}
