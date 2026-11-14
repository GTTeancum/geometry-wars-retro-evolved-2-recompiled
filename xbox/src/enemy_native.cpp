// Native rewrites of the game's per-enemy work in a crowded game (the
// separation is in src/entity_native.cpp), each checked against the
// recompiled original by tools/native_check.cmd.
//
// Same single-precision operations in the same order as the recompiled code,
// double operations kept in double (fmadd as a double multiply then add, as
// gw_fma in the generated code; the single variants rounded once at the end),
// so the results are bit-identical. Guest memory is written only where the
// originals write it (scratch words the originals keep in the red zone below
// the stack pointer are left out).
//
//   0x82052860  the most threatening entity near a point (second spatial hash)
//   0x82046BD8  an enemy's spatial-hash cell, and two flags from its handles
//   0x820631E8  the grid's pushers for the frame (fused with 0x8205A2B0,
//               0x82059FC0 and 0x8205A100; 0x8205A2B0 also on its own)
//   0x8205C5D8  keep a circle inside the arena (and 0x8205C6D8, a box)
//   0x820453E0  Entity::Tick (with the two above inlined)
//   0x82047180  a look-at matrix from forward and up vectors
//   0x8204F458  the nearest live player to an entity
//   0x82040F28  a matrix with its three axes scaled
//   0x82056CD8  a homing enemy's update (colour, chase, turn to its heading)
//   0x82059838  a wanderer's update (spin, drift to the player, jitter)
//   0x82049E60  a pulsing chaser's update
//   0x82056B48  an entity's world matrix (rotation, scale, position fused)
//   0x82045AB8  the player an enemy goes for
//   0x82059CD0  a dodger's update
//   0x82060CC0  a glowing 3D line's vertices
//   0x8205FA98  a glow sprite's vertices
//   0x82045B18  an entity's glow (calls the sprite above)
//   0x82044330  a four-way roller's update
//   0x82054E30  a bouncing drifter's update
//   0x82044168  the roller's world matrix
//   0x82074B28  line debris (the pieces of destroyed enemies)
//   0x82045740  entity drawing (with 0x82045628, 0x82061908, 0x82061A30)
//   0x82030678  a shifting colour
//   0x8212FE70  atan2 (the game's CRT)
//   0x82065E38  the scene's draw (with 0x8206A570 inline)
//   0x8205F6B8  a two-colour 3D line's vertices
//   0x8204D8E0  a blinking entity's update
//   0x82056FA0, 0x82046D88, 0x82047DF8  model drawing
//   0x82045CF0  an entity's world matrix (with 0x82040FF0 inline)
//   0x82030C58  an explosion's burst
//   0x82067A20  the camera (view, projection, shader constants)
//   0x8205EF18  the second line style's vertices
//   0x820366A0  the world update (generated code, registers in locals)
//   0x8207CA48  the score display (generated code, registers in locals)
#include "runtime.h"

#include <xmmintrin.h>

extern "C" {
void sub_8205A3E0(PPCContext& ctx, uint8_t* base);  // the pushers' counting sort (guest)
void sub_8212FE70(PPCContext& ctx, uint8_t* base);  // atan2 (below)
void sub_8203B8B0(PPCContext& ctx, uint8_t* base);  // random vector (guest)
void sub_82045AB8(PPCContext& ctx, uint8_t* base);  // the player an enemy goes for (below)
void sub_82046248(PPCContext& ctx, uint8_t* base);  // separation (src/entity_native.cpp)
void sub_8212EA80(PPCContext& ctx, uint8_t* base);  // cosine (src/math_native.cpp)
void sub_82041600(PPCContext& ctx, uint8_t* base);  // rotation about an axis (src/math_native.cpp)
}

namespace {

constexpr u32 C = 0x82000000;  // constants in the image
constexpr u32 kZero = C + 1740, kOne = C + 1732, kHalf = C + 1828;

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
inline double ldd(u32 a) {
  const u64 v = gw_load64(a);
  double d;
  memcpy(&d, &v, 8);
  return d;
}

// fctiwz on a single-precision value: toward zero, saturating as the PowerPC
// does (NaN and anything below -2^31 give 0x80000000, as cvttss2si).
inline i32 trunc_i32(float v) {
  if (v >= 2147483648.0f) return 0x7FFFFFFF;
  return _mm_cvtt_ss2si(_mm_set_ss(v));
}

// sqrtss (clang would otherwise go through the x87 for __builtin_sqrtf here).
inline float sqrt_ss(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }

// The entity lists the game walks: a head word at +4 pointing at a chain of
// {entity, next} nodes, ended by a node whose entity is 0.
constexpr u32 kPlayers = 0x821813A8;     // players (sub_8204F458, Entity::Tick)
constexpr u32 kPushList1 = 0x8218139C;   // grid pushers, first list
constexpr u32 kPushList2 = 0x821813B4;   // grid pushers, second list
constexpr u32 kEnemyHash = 0x8219E850;   // separation's 32 x 32 hash (its table's address)
constexpr u32 kTargetHash = 0x8219F9AC;  // a second 32 x 32 hash (what the dodgers dodge)
constexpr u32 kDeltaTime = 0x8219D280;   // the frame's time step

// The spatial hashes' cell walk (as in src/entity_native.cpp): the cells
// from (x0, y0) to (x1, y1), wrapping at 32, row by row; a cell's entries
// are a circular list of {entity, next} from +4 of its 16 bytes.
struct CellWalk {
  u32 table, x0, x1, y1, cx, cy, bucket, entry;
  CellWalk(u32 t, u32 ax0, u32 ay0, u32 ax1, u32 ay1)
      : table(t), x0(ax0), x1(ax1), y1(ay1), cx((ax0 - 1) & 31), cy(ay0), bucket(0), entry(0) {
    next_cell();
  }
  // The next cell with anything in it (bucket, its first entry), or entry 0.
  void next_cell() {
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
  }
  void advance() {
    entry = gw_load32(entry + 4);
    if (entry == bucket) next_cell();
  }
};

inline u32 hash_cell(float v) { return (u32)(trunc_i32(v) >> 5) & 31; }

}  // namespace

// ---------------------------------------------------------------------------
// The most threatening entity near a point (guest 0x82052860; the dodger,
// 0x82059CD0, asks it): of the entities in the second hash (0x8219F9AC)
// within f1 of the point r3 (x +0, y +4) and without flags 0x200001 (+36),
// the one with the smallest score: d^2 (1 - (1 + cos a) / 4), a the angle
// between its velocity (+12/+16) and the way from it to the point (so one
// heading for the point scores lowest).
// Returns it in r3 (0 if none), its score in f2 (the image's 0.0 if none);
// f1 is left as the original leaves it (the last candidate's +16), and lr.
extern "C" void sub_82052860(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 point = ctx.r3.u32;
  const double rd = ctx.f1.f64;
  ctx.lr = 0x82052868;  // as the original leaves it (its register-save call)
  const float r2 = (float)(rd * rd);
  float best_score = ldf(kZero);  // (single values throughout: compared as floats)
  const float px = ldf(point + 0);
  const float py0 = ldf(point + 4);
  const u32 x0 = hash_cell((float)((double)px - rd)), y0 = hash_cell((float)((double)py0 - rd));
  const u32 x1 = hash_cell((float)((double)px + rd)), y1 = hash_cell((float)((double)py0 + rd));
  CellWalk w(gw_load32(kTargetHash), x0, y0, x1, y1);
  const float half = ldf(kHalf), one = ldf(kOne);
  u32 best = 0;
  bool any = false;
  float last_vy = 0;
  while (w.entry) {
    const u32 e = gw_load32(w.entry);
    if (!e) break;
    if ((gw_load32(e + 36) & 0x200001) == 0) {
      const float ey = ldf(e + 8);
      const float py = ldf(point + 4);
      const float ex = ldf(e + 4);
      const float dy = py - ey;
      const float dx = px - ex;
      const float dy2 = dy * dy, dx2 = dx * dx;
      const float d2 = dy2 + dx2;
      if (!(d2 > r2)) {
        const float vy = ldf(e + 16), vx = ldf(e + 12);
        any = true;
        last_vy = vy;
        const float inv = one / sqrt_ss(dy2 + dx2);
        const float nx = inv * dx, ny = inv * dy;
        const float invs = one / sqrt_ss(vy * vy + vx * vx);
        float s = (invs * vx) * nx;
        const float t = (invs * vy) * ny;
        s = t + s;
        s = s + one;
        s = s * half;
        // fnmsubs, in double then rounded: -(s/2 - 1). The same as in single
        // precision: s/2 is exact and s is 0 or at least 2^-25 (a sum with
        // 1, halved), so the subtraction is exact in double, or (|s/2| below
        // 2^-29) rounds to 1 either way.
        s = -(s * half - one);
        s = s * d2;
        if (best == 0 || s < best_score) {
          best = e;
          best_score = s;
        }
      }
    }
    w.advance();
  }
  ctx.r3.u64 = best;
  if (any) ctx.f1.f64 = (double)last_vy;
  ctx.f2.f64 = (double)best_score;
}

// ---------------------------------------------------------------------------
// An enemy's hash cell and handle flags (guest 0x82046BD8), r3 the enemy:
// its hash node at +104 {+0 the entity (position +4/+8), +4 next, +8 prev,
// +12 the cell index} is moved to the cell its position is in when that
// changed (unlinked, then put at the end of the new cell's list). Then from
// its handle at +120 (object, generation +124, offset +126; the object's
// generation at +0, pointer at +4): flag 0x1000000 (+36) set when the
// handle's target has +60 above zero, and flag 0x400 set when the target's
// own handle (+4/+8/+10) is live and the byte at 0x8219E84C is set, else
// cleared. The original returns early (no flag work) when its handle is dead.
extern "C" void sub_82046BD8(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 self = ctx.r3.u32;
  const u32 node = self + 104;
  const u32 owner = gw_load32(node + 0);
  const u32 old_cell = gw_load32(node + 12);
  const i32 xi = trunc_i32(ldf(owner + 4));
  const i32 yi = trunc_i32(ldf(owner + 8));
  const u32 cell = (((u32)(xi >> 5)) & 0x1F) | ((u32)yi & 0x3E0);
  if (old_cell != cell) {
    // Unlink, in the original's order of loads and stores.
    gw_store32(gw_load32(node + 4) + 8, gw_load32(node + 8));
    gw_store32(gw_load32(node + 8) + 4, gw_load32(node + 4));
    gw_store32(node + 8, node);
    gw_store32(node + 4, node);
    gw_store32(node + 12, 0xFFFFFFFF);
    // Link at the end of the new cell.
    const u32 bucket = (cell << 4) + gw_load32(kEnemyHash);
    gw_store32(node + 4, bucket);
    gw_store32(node + 8, gw_load32(bucket + 8));
    gw_store32(bucket + 8, node);
    gw_store32(gw_load32(node + 8) + 4, node);
    gw_store32(node + 12, cell);
  }
  // The handle's target, or 0.
  auto resolve = [](u32 handle_object, u32 generation, u32 offset) -> u32 {
    if (gw_load16(handle_object) != generation) return 0;
    const u32 p = gw_load32(handle_object + 4);
    if (!p) return 0;
    return p + offset;
  };
  const u32 target = resolve(gw_load32(self + 120), gw_load16(self + 124), 0);
  if (!target) return;
  const u32 p = target + gw_load16(self + 126);
  if (!p) return;
  // (The original resolves the handle again here; the flag store cannot
  // reach the handle's words, so the result is the same p.)
  if (ldf(p + 60) > ldf(kZero)) gw_store32(self + 36, gw_load32(self + 36) | 0x1000000);
  const u32 h = gw_load32(p + 4);
  bool set = false;
  if (gw_load16(h) == gw_load16(p + 8)) {
    const u32 q = gw_load32(h + 4);
    if (q && gw_load16(p + 10) + q != 0 && gw_load8(0x8219E84C) != 0) set = true;
  }
  if (set) gw_store32(self + 36, gw_load32(self + 36) | 0x400);
  else gw_store32(self + 36, gw_load32(self + 36) & ~0x400u);
}

// ---------------------------------------------------------------------------
// The grid's pushers. The pusher table (r3 of 0x8205A2B0: +12 the count, at
// most 4096; +16 the entries, 20 bytes each: cell, x, y, radius^2, strength)
// gets one entry for every grid cell a circle (r4 its centre, f1 its radius,
// f2 its strength) covers; the grid is K+1556 x K+1560 cells over the
// screen (half size K+1540/K+1544, cells per unit D+9640/D+9644). A circle
// whose centre is off the grid adds nothing. Returns 0 (r3) when the table
// filled up, else 1.
namespace {

constexpr u32 K = 0x821A0000, D = 0x82180000;

// lr, when given, gets what the original leaves in it (its calls' returns).
template <typename R>
bool add_pusher(u32 table, u32 centre, R radius, R strength, u64* lr = nullptr) {
  if (lr) *lr = 0x8205A2B8;
  if ((i32)gw_load32(table + 12) >= 4096) return false;
  if (lr) *lr = 0x8205A2FC;
  // The centre's cell (guest 0x82059FC0).
  {
    const float cx = (ldf(centre + 0) + ldf(K + 1540)) * ldf(D + 9640);
    const float cy = (ldf(centre + 4) + ldf(K + 1544)) * ldf(D + 9644);
    const i32 ix = trunc_i32(cx), iy = trunc_i32(cy);
    if (ix < 0 || iy < 0 || ix >= (i32)gw_load32(K + 1556) || iy >= (i32)gw_load32(K + 1560)) return true;
  }
  if (lr) *lr = 0x8205A31C;
  // The cells covered (guest 0x8205A100): start, columns, rows.
  i32 start, columns, rows;
  {
    const float x = ldf(centre + 0) + ldf(K + 1540);
    const float y = ldf(centre + 4) + ldf(K + 1544);
    const i32 width = (i32)gw_load32(K + 1556);
    const float sx = ldf(D + 9640), sy = ldf(D + 9644);
    const i32 ix0 = trunc_i32((float)((R)x - radius) * sx);
    const i32 ix1 = trunc_i32((float)((R)x + radius) * sx);
    const i32 iy0 = trunc_i32((float)((R)y - radius) * sy);
    const i32 iy1 = trunc_i32((float)((R)y + radius) * sy);
    const i32 wmax = width - 1;
    const i32 x0 = ix0 < 0 ? 0 : (ix0 > wmax ? wmax : ix0);
    const i32 ix1p = (i32)((u32)ix1 + 1), iy1p = (i32)((u32)iy1 + 1);  // wrapping, as addi
    const i32 x1 = ix1p < 0 ? 0 : (ix1p > wmax ? wmax : ix1p);
    const i32 hmax = (i32)gw_load32(K + 1560) - 1;
    const i32 y0 = iy0 < 0 ? 0 : (iy0 > hmax ? hmax : iy0);
    const i32 y1 = iy1p < 0 ? 0 : (iy1p > hmax ? hmax : iy1p);
    start = (i32)((u32)width * (u32)y0 + (u32)x0);
    columns = x1 - x0;
    rows = y1 - y0;
  }
  if (rows <= 0) return true;
  const float r2 = (float)(radius * radius);
  const float s = (float)strength;
  for (i32 row = 0; row < rows; ++row) {
    for (i32 col = 0; col < columns; ++col) {
      const u32 count = gw_load32(table + 12);
      if ((i32)count >= 4096) return false;
      const u32 e = (count * 5) * 4 + gw_load32(table + 16);
      gw_store32(table + 12, count + 1);
      gw_store32(e + 0, (u32)(col + start));
      gw_store32(e + 4, gw_load32(centre + 0));
      gw_store32(e + 8, gw_load32(centre + 4));
      stf(e + 12, r2);
      stf(e + 16, s);
    }
    start += (i32)gw_load32(K + 1556);
  }
  return true;
}

}  // namespace

extern "C" void sub_8205A2B0(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = add_pusher<double>(ctx.r3.u32, ctx.r4.u32, ctx.f1.f64, ctx.f2.f64, &ctx.lr) ? 1 : 0;
}

// The frame's pushers (guest 0x820631E8, r3 the job object; its table at
// +16): emptied, then every entity of the two lists not flagged 0x500031
// (first list) or 0x500011 (second) pushes with its position (+4) and its
// radius (+32) as both size and strength, until the table is full; then the
// game's counting sort by cell (0x8205A3E0, guest).
extern "C" void sub_820631E8(PPCContext& ctx, uint8_t* base) {
  const u32 table = ctx.r3.u32 + 16;
  gw_store32(table + 12, 0);
  // A full table ends a list's walk (the next list is still walked, as the
  // original; it adds nothing more).
  for (int list = 0; list < 2; ++list) {
    const u32 mask = list == 0 ? 0x500031u : 0x500011u;
    u32 node = gw_load32((list == 0 ? kPushList1 : kPushList2) + 4);
    for (u32 e = gw_load32(node); e; e = gw_load32(node)) {
      if ((gw_load32(e + 36) & mask) == 0) {
        const u32 who = gw_load32(node);
        const float r = ldf(who + 32);
        if (!add_pusher<float>(table, who + 4, r, r)) break;
      }
      node = gw_load32(node + 4);
    }
  }
  const u32 sp = ctx.r1.u32;
  ctx.r1.u64 = sp - 112;
  gw_store32(sp - 112, sp);  // the original's frame (its back chain)
  ctx.r3.u64 = table;
  const u64 lr = ctx.lr;
  ctx.lr = 0x820632D8;
  sub_8205A3E0(ctx, base);
  ctx.lr = lr;
  ctx.r1.u64 = sp;
}

// ---------------------------------------------------------------------------
// Keep a circle inside the arena (guest 0x8205C5D8): r3 the position (x +0,
// y +4), f1 the radius, r4 the push vector out. The arena is the screen's
// half size (D+9652, D+9656) less the radius. Inside: returns 0 and touches
// nothing. Outside: the position clamped to the edge, the push vector a unit
// vector away from each crossed edge (0.707 on a corner), returns 1.
// The box version (guest 0x8205C6D8) takes the box from r6 (min) and r7 (max).
namespace {

// The arena (0x8205C5D8): returns whether it pushed.
inline bool keep_in_arena(u32 pos, double radius, u32 out) {
  const float hh = (float)((double)ldf(D + 9656) - radius);
  const float y = ldf(pos + 4), x = ldf(pos + 0);
  const float hw = (float)((double)ldf(D + 9652) - radius);
  const double f8 = (double)(y + hh), f3 = (double)(hh - y);
  const double f5 = (double)(hw - x), f4 = (double)(x + hw);
  const double zero = ldd(C + 3376);
  double f10 = f8 >= 0.0 ? f3 : f8;
  f10 = f5 >= 0.0 ? f10 : f5;
  const double f9a = f4 >= 0.0 ? f10 : f4;
  if (!(f9a < zero)) return false;
  const float f6 = -hh, f7n = -hw;
  const float f2 = y - hh;
  const double minus_one = ldd(C + 3200), one = ldd(C + 3192);
  const double f5b = f5 >= 0.0 ? zero : minus_one;
  const double f3b = f3 >= 0.0 ? zero : minus_one;
  const double px = f4 >= 0.0 ? f5b : one;
  const float f4b = f6 - y;
  const float f5c = f7n - x;
  const double py = f8 >= 0.0 ? f3b : one;
  const float f3c = x - hw;
  float ny = f4b >= 0 ? f6 : y;
  float nx = f5c >= 0 ? f7n : x;
  const double px2 = px * px;
  ny = f2 >= 0 ? hh : ny;
  nx = f3c >= 0 ? hw : nx;
  const double len2 = py * py + px2;
  stf(pos + 4, ny);
  stf(pos + 0, nx);
  const double k = (len2 - ldd(C + 4648)) >= 0.0 ? ldd(C + 10152) : one;
  stf(out + 0, (float)(k * px));
  stf(out + 4, (float)(k * py));
  return true;
}

// The box (0x8205C6D8).
inline bool keep_in_box(u32 pos, double radius, u32 out, u32 lo, u32 hi) {
  const float top = (float)((double)ldf(lo + 4) + radius);    // f9
  const float right = (float)((double)ldf(hi + 0) - radius);  // f8
  const float left = (float)((double)ldf(lo + 0) + radius);   // f7
  const float bottom = (float)((double)ldf(hi + 4) - radius); // f6
  const float y = ldf(pos + 4), x = ldf(pos + 0);
  const double f10 = (double)(y - top), f5 = (double)(right - x);
  const double f4 = (double)(x - left), f3 = (double)(bottom - y);
  const double zero = ldd(C + 3376);
  double m = f10 >= 0.0 ? f3 : f10;
  m = f5 >= 0.0 ? m : f5;
  m = f4 >= 0.0 ? m : f4;
  if (!(m < zero)) return false;
  const float f2 = y - bottom;
  const double minus_one = ldd(C + 3200), one = ldd(C + 3192);
  const double f5b = f5 >= 0.0 ? zero : minus_one;
  const double f3b = f3 >= 0.0 ? zero : minus_one;
  const double px = f4 >= 0.0 ? f5b : one;
  const float f4b = top - y;
  const float f5c = left - x;
  const double py = f10 >= 0.0 ? f3b : one;
  const float f3c = x - right;
  float ny = f4b >= 0 ? top : y;
  float nx = f5c >= 0 ? left : x;
  const double px2 = px * px;
  ny = f2 >= 0 ? bottom : ny;
  nx = f3c >= 0 ? right : nx;
  const double len2 = py * py + px2;
  stf(pos + 4, ny);
  stf(pos + 0, nx);
  const double k = (len2 - ldd(C + 4648)) >= 0.0 ? ldd(C + 10152) : one;
  stf(out + 0, (float)(k * px));
  stf(out + 4, (float)(k * py));
  return true;
}

}  // namespace

extern "C" void sub_8205C5D8(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = keep_in_arena(ctx.r3.u32, ctx.f1.f64, ctx.r4.u32) ? 1 : 0;
}

extern "C" void sub_8205C6D8(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = keep_in_box(ctx.r3.u32, ctx.f1.f64, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Entity::Tick (guest 0x820453E0), r3 the entity: vtable at +0, position
// +4/+8, velocity +12/+16, radius +32, flags +36, a fade timer +56, the
// arena index +168. In order:
//  - flag 0x10: the timer runs up by 1.5 x the time step; at 1 it stops and
//    the flag goes; while it runs nothing else below happens but the clamp;
//  - the virtual update (vtable +12); then, unless the byte at 0x8219E7BC is
//    set and neither flag 2 nor 8 is, the position moves by the velocity;
//  - flag 0x800: the virtual +16;
//  - kept in the arena (flag 2: the arena box of table 0x8219CEB4, 108
//    bytes an entry, when its byte +68 is set; else the screen), unless
//    flag 8 is clear and 0x800000 set; pushed: the virtual +20 with the push;
//  - when it was not fading and flags & 0x144 are 0x104: every player
//    (flag 1 clear) touching it gets its virtual +48 with this entity.
// The virtual calls go through the game's dispatch, as the original's.
extern "C" void sub_820453E0(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  // ctx.lr is left as the original leaves it: its last call's return (the
  // original's epilogue does not restore it).
  ctx.lr = 0x820453E8;
  const u32 sp = ctx.r1.u32, frame = sp - 128;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  // ctx.lr as the original sets it for each call.
  auto vcall = [&](u32 object, u32 slot, u32 return_address) {
    const u32 target = gw_load32(gw_load32(object) + slot);
    ctx.lr = return_address;
    REX_CALL_INDIRECT_FUNC(target);
  };
  const u32 flags0 = gw_load32(self + 36);
  const bool fading = (flags0 >> 4) & 1;
  if (fading) {
    const float t = (float)((double)ldf(kDeltaTime) * (double)ldf(C + 3216) + (double)ldf(self + 56));
    stf(self + 56, t);
    const float one = ldf(kOne);
    if (!(t < one)) {
      stf(self + 56, one);
      gw_store32(self + 36, flags0 & ~0x10u);
    }
  }
  if (!(gw_load32(self + 36) & 0x10)) {
    ctx.r3.u64 = self;
    vcall(self, 12, 0x8204545C);
    if (gw_load8(0x8219E7BC) == 0 || (gw_load32(self + 36) & 10) != 0) {
      const float dt = ldf(kDeltaTime);
      const float mx = ldf(self + 12) * dt;
      const float my = ldf(self + 16) * dt;
      const float y = ldf(self + 8);
      stf(self + 4, ldf(self + 4) + mx);
      stf(self + 8, my + y);
    }
    if (gw_load32(self + 36) & 0x800) {
      ctx.r3.u64 = self;
      vcall(self, 16, 0x820454D0);
    }
  }
  {
    const u32 flags = gw_load32(self + 36);
    const float zero = ldf(kZero);
    const u32 push = frame + 80;
    stf(push + 0, zero);
    stf(push + 4, zero);
    bool clamp = true, box = false;
    u32 arena = 0;
    if (flags & 2) {
      arena = gw_load32(self + 168) * 108 + 0x8219CEB4;
      box = gw_load8(arena + 68) != 0;
    } else if (!(flags & 8) && (flags & 0x800000)) {
      clamp = false;
    }
    if (clamp) {
      const double radius = (double)ldf(self + 32);
      bool pushed;
      ctx.lr = box ? 0x8204552C : 0x82045558;
      if (box) {
        pushed = keep_in_box(self + 4, radius, push, arena + 72, arena + 80);
        ctx.r6.u64 = arena + 72;
        ctx.r7.u64 = arena + 80;
      } else {
        pushed = keep_in_arena(self + 4, radius, push);
      }
      if (pushed) {
        ctx.r3.u64 = self;
        ctx.r4.u64 = push;
        vcall(self, 20, 0x8204557C);
      }
    }
  }
  if (!fading && (gw_load32(self + 36) & 324) == 260) {
    u32 node = gw_load32(kPlayers + 4);
    for (u32 other = gw_load32(node); other; other = gw_load32(node)) {
      if (!(gw_load32(other + 36) & 1)) {
        const u32 o = gw_load32(node);
        const float sum = ldf(o + 32) + ldf(self + 32);
        const float dx = ldf(o + 4) - ldf(self + 4);
        const float dy = ldf(o + 8) - ldf(self + 8);
        const float reach = sum * sum;
        const float d2 = dy * dy + dx * dx;
        if (!(d2 > reach)) {
          ctx.r3.u64 = other;
          ctx.r4.u64 = self;
          vcall(other, 48, 0x82045610);
        }
      }
      node = gw_load32(node + 4);
    }
  }
  ctx.r1.u64 = sp;
}

// ---------------------------------------------------------------------------
// Look-at matrix (guest 0x82047180): r3 the 4 x 4 out, r4 the position (row
// 3, copied), r5 forward and r6 up (4 floats each). Row 2 the forward
// normalised, row 1 up less its part along forward, normalised, row 0 their
// cross product normalised (its w zero times the scale).
extern "C" void sub_82047180(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 out = ctx.r3.u32, pos = ctx.r4.u32, fwd = ctx.r5.u32, up = ctx.r6.u32;
  const float a2 = ldf(fwd + 8), a3 = ldf(fwd + 12), a0 = ldf(fwd + 0), a1 = ldf(fwd + 4);
  float b2 = ldf(up + 8), b3 = ldf(up + 12), b1 = ldf(up + 4), b0 = ldf(up + 0);
  const float one = ldf(kOne);
  float len = a3 * a3 + a2 * a2;
  len = len + a0 * a0;
  len = len + a1 * a1;
  float inv = one / sqrt_ss(len);
  const float fz = a2 * inv, fw = a3 * inv, fy = a1 * inv, fx = a0 * inv;
  float dot = b3 * fw + b2 * fz;
  dot = dot + b1 * fy;
  dot = dot + b0 * fx;
  b2 = b2 - fz * dot;
  b3 = b3 - fw * dot;
  b1 = b1 - fy * dot;
  b0 = b0 - dot * fx;
  float l2 = b3 * b3 + b2 * b2;
  l2 = l2 + b1 * b1;
  l2 = l2 + b0 * b0;
  inv = one / sqrt_ss(l2);
  const float uy = b1 * inv, ux = b0 * inv, uz = b2 * inv, uw = b3 * inv;
  const float t4 = uy * fx, t11 = fz * ux, t3 = uz * fy;
  const float c2 = (float)((double)fy * (double)ux - (double)t4);
  const float c1 = (float)((double)uz * (double)fx - (double)t11);
  const float c0 = (float)((double)uy * (double)fz - (double)t3);
  float n2 = c2 * c2 + c1 * c1;
  n2 = n2 + c0 * c0;
  inv = one / sqrt_ss(n2);
  stf(out + 0, inv * c0);
  stf(out + 4, c1 * inv);
  stf(out + 8, c2 * inv);
  stf(out + 12, inv * ldf(kZero));
  stf(out + 16, ux);
  stf(out + 20, uy);
  stf(out + 24, uz);
  stf(out + 28, uw);
  stf(out + 32, fx);
  stf(out + 36, fy);
  stf(out + 40, fz);
  stf(out + 44, fw);
  for (u32 k = 0; k < 16; k += 4) gw_store32(out + 48 + k, gw_load32(pos + k));
  ctx.f1.f64 = (double)(c2 * c2);
}

// ---------------------------------------------------------------------------
// The nearest player (guest 0x8204F458): of the players list (flag 1 clear,
// +224 not above zero, not r3 itself) the one nearest r3 (+4/+8), in r3;
// 0 if none.
extern "C" void sub_8204F458(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 self = ctx.r3.u32;
  const float zero = ldf(kZero);
  float best_d = zero;
  u32 best = 0;
  u32 node = gw_load32(kPlayers + 4);
  for (u32 e = gw_load32(node); e; e = gw_load32(node)) {
    if (!(gw_load32(e + 36) & 1) && !(ldf(e + 224) > zero) && e != self) {
      const float dx = ldf(self + 4) - ldf(e + 4);
      const float dy = ldf(self + 8) - ldf(e + 8);
      const float d = dx * dx + dy * dy;
      if (best == 0 || d < best_d) {
        best = e;
        best_d = d;
      }
    }
    node = gw_load32(node + 4);
  }
  ctx.r3.u64 = best;
}

// ---------------------------------------------------------------------------
// Scaled matrix (guest 0x82040F28): r3 out = r4 with rows 0-2 times the
// scale r5 (x, y, z), row 3 copied. Everything read before the first store,
// as the original (r3 may be r4).
extern "C" void sub_82040F28(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 out = ctx.r3.u32, m = ctx.r4.u32, s = ctx.r5.u32;
  float v[12];
  const float sx = ldf(s + 0);
  for (u32 i = 0; i < 4; ++i) v[i] = ldf(m + i * 4) * sx;
  const float sy = ldf(s + 4);
  for (u32 i = 4; i < 8; ++i) v[i] = ldf(m + i * 4) * sy;
  const float sz = ldf(s + 8);
  for (u32 i = 8; i < 12; ++i) v[i] = ldf(m + i * 4) * sz;
  for (u32 i = 0; i < 12; ++i) stf(out + i * 4, v[i]);
  for (u32 k = 48; k < 64; k += 4) gw_store32(out + k, gw_load32(m + k));
}

// ---------------------------------------------------------------------------
// A homing enemy's update (guest 0x82056CD8, a virtual; r3 the enemy). Its
// colour (+40..+52) goes from the base colour (0x821A0058) toward (5, 4, 3, 1)
// by t^3 (t +144, falling by 0.2 a frame to 0), the colour (not the alpha)
// times the brightness +148. With a live target (+72, kind 13 at +68; aiming
// at its +160 if set, else at the target) more than 1 away: a pull of 36 x
// (d/8 clamped to [0.5, 2]) toward it, the velocity (+12/+16) damped by 0.8,
// a fifth of the aim's velocity difference added, and the heading +20 the
// direction's atan2 (guest CRT); closer, the aim's heading. Without one its
// timer +140 runs down and, run out, the virtual +36 hears of it (with
// {self, self, 3 or 0 by the byte +152}); the velocity damped by 0.8.
extern "C" void sub_82056CD8(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 112;
  {
    const u32 tint = 0x821A0038;
    const float t = ldf(self + 144);
    const float t3 = (t * t) * t;
    const float a0 = ldf(tint + 32), a1 = ldf(tint + 36), a2 = ldf(tint + 40), a3 = ldf(tint + 44);
    const float k = ldf(self + 148);
    const float d0 = ldf(C + 1776) - a0, d1 = ldf(C + 1780) - a1, d2 = ldf(C + 1784) - a2;
    const float d3 = ldf(kOne) - a3;
    const float c0 = d0 * t3 + a0, c1 = d1 * t3 + a1, c2 = d2 * t3 + a2, c3 = d3 * t3 + a3;
    stf(self + 52, c3);
    stf(self + 40, c0 * k);
    stf(self + 44, c1 * k);
    stf(self + 48, c2 * k);
  }
  const float fifth = ldf(C + 3420), zero = ldf(kZero);
  {
    const float t = ldf(self + 144) - fifth;
    stf(self + 144, t >= 0 ? t : zero);
  }
  const u32 target = gw_load32(self + 72);
  if (target && gw_load32(target + 68) == 13) {
    u32 aim = gw_load32(self + 160);
    if (!aim) aim = target;
    const float dx = ldf(aim + 4) - ldf(self + 4);
    const float dy = ldf(aim + 8) - ldf(self + 8);
    const float len2 = dy * dy + dx * dx;
    const float q = len2 + ldf(C + 9080);
    // frsqrte and fres as the recompiled code does them: exact in double,
    // rounded once.
    const float rsq = (float)(1.0 / __builtin_sqrt((double)q));
    const float dist = (float)(1.0 / (double)rsq);
    const float nx = rsq * dx, ny = dy * rsq;
    if (!(dist > ldf(kOne))) {
      gw_store32(self + 20, gw_load32(aim + 20));
      return;
    }
    const float d8 = dist * ldf(C + 4612);
    const float lo = ldf(kHalf) - d8;
    const float hi = d8 - ldf(C + 1788);
    double pull = lo >= 0 ? ldd(C + 9072) : (double)d8;
    pull = hi >= 0 ? ldd(C + 4480) : pull;
    const float f = (float)pull * ldf(C + 8364);
    const float vy0 = ldf(self + 16);
    const float px = f * nx, py = ny * f;
    const float vx = ldf(self + 12) + px;
    stf(self + 12, vx);
    const float vy = vy0 + py;
    stf(self + 16, vy);
    const float damp = ldf(C + 1876);
    const float wx = vx * damp;
    stf(self + 12, wx);
    stf(self + 16, vy * damp);
    const float ex = ldf(aim + 12) - wx;
    const float ey = ldf(aim + 16) - ldf(self + 16);
    const float ux = ldf(self + 12), uy = ldf(self + 16);
    stf(self + 12, ux + ex * fifth);
    stf(self + 16, uy + ey * fifth);
    ctx.r1.u64 = frame;
    gw_store32(frame, sp);
    ctx.f1.f64 = (double)ny;
    ctx.f2.f64 = (double)nx;
    ctx.lr = 0x82056EE0;
    sub_8212FE70(ctx, base);
    stf(self + 20, (float)ctx.f1.f64);
    ctx.r1.u64 = sp;
    ctx.lr = lr;
    return;
  }
  const float timer = ldf(self + 140) - ldf(kDeltaTime);
  stf(self + 140, timer);
  if (!(timer > zero)) {
    ctx.r1.u64 = frame;
    gw_store32(frame, sp);
    gw_store32(frame + 80, self);
    gw_store32(frame + 84, self);
    gw_store32(frame + 88, gw_load8(self + 152) ? 3 : 0);
    ctx.r3.u64 = self;
    ctx.r4.u64 = frame + 80;
    ctx.lr = 0x82056F68;
    const u32 fn = gw_load32(gw_load32(self) + 36);
    REX_CALL_INDIRECT_FUNC(fn);
    ctx.r1.u64 = sp;
    ctx.lr = lr;
  }
  const float damp = ldf(C + 1876);
  const float vy = ldf(self + 16);
  stf(self + 12, ldf(self + 12) * damp);
  stf(self + 16, vy * damp);
}

// ---------------------------------------------------------------------------
// A wanderer's update (guest 0x82059838, r3 the enemy): its colour from
// 0x821A059C, spin +128 added to the angle +20; a pull of 0.1 toward the
// player (0x82045AB8, below) if there is one, a random kick of 5 (the game's
// generator, 0x8203B8B0), the speed eased 5% toward 50, then separation
// (radius +32, strength 1).
extern "C" void sub_82059838(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 112;
  for (u32 k = 0; k < 16; k += 4) gw_store32(self + 40 + k, gw_load32(0x821A057C + 32 + k));
  stf(self + 20, ldf(self + 128) + ldf(self + 20));
  ctx.r1.u64 = frame;
  gw_store32(frame, sp);
  ctx.lr = 0x8205988C;
  sub_82045AB8(ctx, base);
  const float one = ldf(kOne);
  const u32 player = ctx.r3.u32;
  if (player) {
    const float dx = ldf(player + 4) - ldf(self + 4);
    const float dy = ldf(player + 8) - ldf(self + 8);
    const float vx = ldf(self + 12), vy = ldf(self + 16);
    const float inv = one / sqrt_ss(dy * dy + dx * dx);
    const float k = ldf(C + 1812);
    const float nx = inv * dx, ny = inv * dy;
    stf(self + 12, vx + nx * k);
    stf(self + 16, vy + ny * k);
  }
  ctx.r3.u64 = frame + 80;
  ctx.r4.u64 = 0x821A1E28;
  ctx.f1.f64 = (double)ldf(C + 1776);
  ctx.lr = 0x82059914;
  sub_8203B8B0(ctx, base);
  const u32 kick = ctx.r3.u32;
  stf(self + 12, ldf(kick + 0) + ldf(self + 12));
  stf(self + 16, ldf(kick + 4) + ldf(self + 16));
  {
    const float vx = ldf(self + 12), vy = ldf(self + 16);
    const float ease = ldf(C + 3424);
    const float speed = sqrt_ss(vy * vy + vx * vx);
    const float gap = ldf(C + 4160) - speed;
    const float aim = (float)((double)gap * (double)ease + (double)speed);
    const float scale = aim / speed;
    stf(self + 12, vx * scale);
    stf(self + 16, vy * scale);
  }
  ctx.r3.u64 = self;
  ctx.f1.f64 = (double)ldf(self + 32);
  ctx.f2.f64 = (double)one;
  ctx.lr = 0x82059998;
  sub_82046248(ctx, base);
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// A pulsing chaser's update (guest 0x82049E60, r3 the enemy): its phase
// +132 runs at 5 a second, +136 its cosine; with a player (0x82045AB8,
// below) it accelerates by +128 toward it (when more than 0.001 away), is
// damped by 0.9, and +128 grows by 0.005; then separation (radius +32,
// strength 1); its scale +24/+28 is 1 +- 0.2 x the cosine.
extern "C" void sub_82049E60(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 112;
  ctx.r1.u64 = frame;
  gw_store32(frame, sp);
  const float phase = (float)((double)ldf(kDeltaTime) * (double)ldf(C + 1776) + (double)ldf(self + 132));
  stf(self + 132, phase);
  ctx.f1.f64 = (double)phase;
  ctx.lr = 0x82049E98;
  sub_8212EA80(ctx, base);
  stf(self + 136, (float)ctx.f1.f64);
  ctx.r3.u64 = self;
  ctx.lr = 0x82049EA8;
  sub_82045AB8(ctx, base);
  const float one = ldf(kOne);
  const u32 player = ctx.r3.u32;
  if (player) {
    const float dx = ldf(player + 4) - ldf(self + 4);
    const float dy = ldf(player + 8) - ldf(self + 8);
    const float d = sqrt_ss(dx * dx + dy * dy);
    const float inv = one / d;
    if (d > ldf(C + 6680)) {
      const float nx = inv * dx, ny = inv * dy;
      const float a = ldf(self + 128);
      const float vy = ldf(self + 16);
      const float px = a * nx, py = ny * a;
      stf(self + 12, ldf(self + 12) + px);
      stf(self + 16, vy + py);
    }
    const float damp = ldf(C + 3440);
    const float vy = ldf(self + 16);
    stf(self + 12, ldf(self + 12) * damp);
    stf(self + 16, vy * damp);
    stf(self + 128, ldf(self + 128) + ldf(C + 6996));
  }
  ctx.r3.u64 = self;
  ctx.f1.f64 = (double)ldf(self + 32);
  ctx.f2.f64 = (double)one;
  ctx.lr = 0x82049F64;
  sub_82046248(ctx, base);
  const float w = ldf(self + 136) * ldf(C + 3420);
  stf(self + 24, w + one);
  stf(self + 28, one - w);
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// An entity's world matrix (guest 0x82056B48): r3 the 4 x 4 out, r4 the
// entity (position +4/+8, angle +20, size +28). The original builds a
// rotation about z (0x82041600; here too, into its frame as the original), a
// copy with the position added, a scale (1, 2 size, 2 size) of the identity
// (0x82040F28) and their product (0x82041CA0); here the copy, the scale and
// the product are done in registers with the same operations (the product's
// as src/math_native.cpp's). Returns r3.
extern "C" void sub_82056B48(PPCContext& ctx, uint8_t* base) {
  const u32 out = ctx.r3.u32, ent = ctx.r4.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 416;
  ctx.r1.u64 = frame;
  gw_store32(frame, sp);
  const float zero = ldf(kZero), one = ldf(kOne);
  const float px = ldf(ent + 4), py = ldf(ent + 8);
  const u32 axis = frame + 80, rot = frame + 224;
  stf(axis + 0, zero);
  stf(axis + 4, zero);
  stf(axis + 8, one);
  stf(axis + 12, zero);
  ctx.r3.u64 = rot;
  ctx.r4.u64 = axis;
  ctx.f1.f64 = (double)ldf(ent + 20);
  ctx.lr = 0x82056BA4;
  sub_82041600(ctx, base);
  // The rotation with the position added to its last row.
  float m[16];
  for (u32 i = 0; i < 16; ++i) m[i] = ldf(rot + i * 4);
  m[12] = m[12] + px;
  m[13] = m[13] + py;
  // The scale: the identity's rows times (1, s, s), its last row copied.
  const float s = ldf(ent + 28) * ldf(C + 1788);
  const float id[16] = {one, zero, zero, zero, zero, one, zero, zero, zero, zero, one, zero, zero, zero, zero, one};
  float sc[16];
  for (u32 i = 0; i < 4; ++i) sc[i] = id[i] * one;
  for (u32 i = 4; i < 12; ++i) sc[i] = id[i] * s;
  for (u32 i = 12; i < 16; ++i) sc[i] = id[i];
  // out = scale x m, row by row.
  for (u32 row = 0; row < 4; ++row) {
    const float v0 = sc[row * 4], v1 = sc[row * 4 + 1], v2 = sc[row * 4 + 2], v3 = sc[row * 4 + 3];
    for (u32 c = 0; c < 4; ++c) {
      float t = v0 * m[c] + v1 * m[4 + c];
      t = t + v2 * m[8 + c];
      stf(out + row * 16 + c * 4, t + v3 * m[12 + c]);
    }
  }
  ctx.r3.u64 = out;
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// The player an enemy goes for (guest 0x82045AB8, r3 the enemy): with no
// current target (+72 zero) the nearest player (0x8204F458, above); else
// the first live player (flag 1 clear) that is not the current target, or 0.
extern "C" void sub_82045AB8(PPCContext& ctx, uint8_t* base) {
  const u32 current = gw_load32(ctx.r3.u32 + 72);
  if (!current) {
    sub_8204F458(ctx, base);
    return;
  }
  u32 node = gw_load32(kPlayers + 4);
  for (u32 e = gw_load32(node); e; e = gw_load32(node)) {
    if (!(gw_load32(e + 36) & 1) && e != current) {
      ctx.r3.u64 = gw_load32(node);
      return;
    }
    node = gw_load32(node + 4);
  }
  ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// A dodger's update (guest 0x82059CD0, r3 the enemy): its colour from
// 0x821A05E0; with a player (0x82045AB8): the nearest threat within 64 of it
// (0x82052860, the seekers' hash) pushes it sideways, 90 x a weight of how
// squarely the threat heads at it (cos a from its velocity and the way to
// the enemy: h = (1 + cos a) / 2; above 0.95 the weight 1 - 20 (h - 0.95),
// above 0.375 sqrt h, else 0); its turn timer +128 runs down and, run out,
// a new turn rate +132 (random up to 9) and timer (0.2 (1 + random / 2^32))
// from the game's generator (0x82030220, 0x82030058; guest); toward the
// player (more than 0.001 away) a pull of 18 and a sideways swerve of +132,
// damped by 0.9. Always: the angle +20 grows by 0.01 (speed x step)^2, then
// separation (radius +32, strength 1).
extern "C" void sub_82030220(PPCContext& ctx, uint8_t* base);  // random float (guest)
extern "C" void sub_82030058(PPCContext& ctx, uint8_t* base);  // random integer (guest)
extern "C" void sub_82059CD0(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;  // (ctx.lr left at the last call's return, as the original)
  const u32 sp = ctx.r1.u32, frame = sp - 160;
  ctx.r1.u64 = frame;
  gw_store32(frame, sp);
  for (u32 k = 0; k < 16; k += 4) gw_store32(self + 40 + k, gw_load32(0x821A05C0 + 32 + k));
  ctx.lr = 0x82059D18;
  sub_82045AB8(ctx, base);
  const u32 player = ctx.r3.u32;
  const float one = ldf(kOne);
  if (player) {
    const u32 pos = self + 4;
    ctx.r3.u64 = pos;
    ctx.f1.f64 = (double)ldf(C + 6668);
    ctx.lr = 0x82059D44;
    sub_82052860(ctx, base);
    const u32 threat = ctx.r3.u32;
    const float zero = ldf(kZero);
    if (threat) {
      const float dx = ldf(pos + 0) - ldf(threat + 4);
      const float dy = ldf(pos + 4) - ldf(threat + 8);
      const float vy = ldf(threat + 16), vx = ldf(threat + 12);
      const float vx2 = vx * vx, vy2 = vy * vy, dx2 = dx * dx, dy2 = dy * dy;
      const float invd = one / sqrt_ss(dy2 + dx2);
      const float invv = one / sqrt_ss(vy2 + vx2);
      const float nx = invd * dx, ny = invd * dy;
      const float ux = invv * vx, uy = vy * invv;
      const float c = (uy * ny) + (ux * nx);
      const float h = (c + one) * ldf(kHalf);
      const float high = ldf(C + 4468);
      float w;
      if (h > high) w = (float)(-((double)(h - high) * (double)ldf(C + 3304) - (double)one));
      else if (h > ldf(C + 8956)) w = sqrt_ss(h);
      else w = zero;
      w = w * ldf(C + 4352);
      const float px = nx * w, py = ny * w;
      const float vy0 = ldf(self + 16);
      stf(self + 12, ldf(self + 12) + px);
      stf(self + 16, vy0 + py);
    }
    const float timer = ldf(self + 128) - ldf(kDeltaTime);
    const float tx = ldf(player + 4) - ldf(pos + 0);
    const float ty = ldf(player + 8) - ldf(pos + 4);
    stf(self + 128, timer);
    if (!(timer > zero)) {
      ctx.r3.u64 = 0x821A1E28;
      ctx.f1.f64 = (double)ldf(C + 6932);
      ctx.lr = 0x82059E84;
      sub_82030220(ctx, base);
      stf(self + 132, (float)ctx.f1.f64);
      ctx.r3.u64 = 0x821A1E28;
      ctx.lr = 0x82059E90;
      sub_82030058(ctx, base);
      gw_store64(frame + 80, ctx.r3.u64 & 0xFFFFFFFF);  // (the original's conversion slot)
      const float r = (float)(double)(u32)ctx.r3.u64;
      const float t = (float)((double)r * (double)ldf(C + 1764) + (double)one);
      stf(self + 128, t * ldf(C + 3420));
    }
    const float d = sqrt_ss(ty * ty + tx * tx);
    const float inv = one / d;
    const float nx = inv * tx, ny = inv * ty;
    if (d > ldf(C + 6680)) {
      const float k = ldf(C + 10024);
      const float vy = ldf(self + 16);
      stf(self + 12, nx * k + ldf(self + 12));
      stf(self + 16, ny * k + vy);
      const float turn = ldf(self + 132);
      const float sx = ny * turn, sy = -nx * turn;
      const float wy = ldf(self + 16);
      stf(self + 12, ldf(self + 12) + sx);
      stf(self + 16, wy + sy);
    }
    const float damp = ldf(C + 3440);
    const float vy = ldf(self + 16);
    stf(self + 12, ldf(self + 12) * damp);
    stf(self + 16, vy * damp);
  }
  {
    const float vx = ldf(self + 12), vy = ldf(self + 16);
    const float angle = ldf(self + 20);
    const float speed = sqrt_ss(vy * vy + vx * vx);
    float s = speed * ldf(kDeltaTime);
    s = s * s;
    stf(self + 20, (float)((double)s * (double)ldf(C + 4528) + (double)angle));
  }
  ctx.r3.u64 = self;
  ctx.f1.f64 = (double)ldf(self + 32);
  ctx.f2.f64 = (double)one;
  ctx.lr = 0x82059FA8;
  sub_82046248(ctx, base);
  ctx.r1.u64 = sp;
}

// ---------------------------------------------------------------------------
// A glowing 3D line (guest 0x82060CC0): r3 and r4 its ends (x, y, z), f1
// its width, r5 its colour (r, g, b, a). Four vertices from the stream at
// 0x821A4108 (the glow quad) and eight from 0x821A4948 (the outline),
// through the game's allocator (0x82061750, guest), each 8 floats (position,
// -1, the colour times its alpha times 8, 1), written big-endian into the
// game's vertex chunk as the original writes them. The ends' direction is
// normalised (1 / sqrt in double, plus 1e-5) for the sideways offsets, the
// half width scaled by 0x821A1DB4. The arithmetic below follows the
// original instruction by instruction (fN_k: the k-th value of its
// register fN; single values in float, fmadd/fnmsub and fmul in double,
// rounded where it rounds), which the checker confirms bit for bit.
extern "C" void sub_82061750(PPCContext& ctx, uint8_t* base);  // vertices from a dynamic stream (guest)
extern "C" void sub_82060CC0(PPCContext& ctx, uint8_t* base) {
  const double width = ctx.f1.f64;
  const u32 r29 = ctx.r3.u32, r28 = ctx.r4.u32, r30 = ctx.r5.u32;
  const u32 sp = ctx.r1.u32, frame = sp - 192;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  ctx.r3.u64 = 0x821A4108;
  ctx.r4.u64 = 4;
  ctx.lr = 0x82060CF4;
  sub_82061750(ctx, base);
  const u32 r31 = ctx.r3.u32;
  ctx.r4.u64 = 8;
  ctx.r3.u64 = 0x821A4948;
  ctx.lr = 0x82060D0C;
  sub_82061750(ctx, base);
  const u32 r3 = ctx.r3.u32;
  u32 r10, r11;
  r11 = (u32)-2112225280;
  const float f12_1 = ldf(r30 + 12);
  r11 = r11 + 7572;
  const float f11_1 = ldf(r30 + 0);
  const float f10_1 = ldf(r30 + 4);
  const float f11_2 = f12_1 * f11_1;
  const float f9_1 = ldf(r30 + 8);
  const float f8_1 = f10_1 * f12_1;
  const float f7_1 = ldf(r29 + 0);
  const float f9_2 = f9_1 * f12_1;
  const float f5_1 = ldf(r28 + 0);
  const float f0_1 = ldf(r11 + 32);
  r11 = (u32)-2113929216;
  const float f31_1 = f5_1 - f7_1;
  const float f6_1 = ldf(r29 + 4);
  const float f4_1 = ldf(r28 + 4);
  const float f3_1 = ldf(r29 + 8);
  const float f30_1 = f4_1 - f6_1;
  const float f2_1 = ldf(r28 + 8);
  const float f1_1 = ldf(r11 + 3208);
  r11 = (u32)-2113929216;
  const float f29_1 = f2_1 - f3_1;
  const float f13_1 = ldf(r11 + 1732);
  r11 = (u32)-2113929216;
  const float f10_2 = ldf(r11 + 3352);
  r11 = (u32)-2113929216;
  const float f12_2 = f11_2 * f10_2;
  const float f11_3 = f8_1 * f10_2;
  const float f10_3 = f9_2 * f10_2;
  const double f9_3 = (double)f31_1 * (double)f31_1;
  const double f8_2 = (double)f30_1 * (double)f30_1;
  const double f27_1 = ((double)f29_1 * (double)f29_1 + f9_3);
  const double f26_1 = f9_3 + f8_2;
  const double f9_4 = ldd(r11 + 10080);
  r11 = r31 + 32;
  const double f8_3 = f27_1 + f8_2;
  const double f27_2 = f26_1 + f9_4;
  const double f8_4 = f8_3 + f9_4;
  const float f9_5 = (float)(1.0 / __builtin_sqrt(f27_2));
  const float f8_5 = (float)(1.0 / __builtin_sqrt(f8_4));
  const double f28_1 = (double)f8_5 * width;
  const double f8_6 = (double)f9_5 * (double)f31_1;
  const double f9_6 = (double)f9_5 * (double)f30_1;
  const double f31_2 = f28_1 * (double)f31_1;
  const double f8_7 = -f8_6;
  const double f30_2 = f28_1 * (double)f30_1;
  const double f29_2 = f28_1 * (double)f29_1;
  const double f28_2 = -((double)f0_1 * f9_6 + -(double)f7_1);
  const double f27_3 = ((double)f0_1 * f9_6 + (double)f7_1);
  const double f26_2 = -((double)f0_1 * f8_7 + -(double)f6_1);
  const float f28_3 = (float)f28_2;
  stf(r31 + 0, f28_3);
  const float f28_4 = (float)f27_3;
  const float f27_4 = (float)f26_2;
  stf(r31 + 4, f27_4);
  stf(r31 + 8, f3_1);
  const double f27_5 = ((double)f0_1 * f9_6 + (double)f5_1);
  stf(r31 + 12, f1_1);
  stf(r31 + 16, f12_2);
  stf(r31 + 20, f11_3);
  stf(r31 + 24, f10_3);
  stf(r31 + 28, f13_1);
  stf(r11 + 0, f28_4);
  const double f28_5 = ((double)f0_1 * f8_7 + (double)f6_1);
  const float f28_6 = (float)f28_5;
  stf(r11 + 4, f28_6);
  stf(r11 + 8, f3_1);
  stf(r11 + 12, f13_1);
  const double f28_7 = ((double)f0_1 * f8_7 + (double)f4_1);
  stf(r11 + 16, f12_2);
  const double f26_3 = -((double)f0_1 * f9_6 + -(double)f5_1);
  stf(r11 + 20, f11_3);
  const double f25_1 = -((double)f0_1 * f8_7 + -(double)f4_1);
  stf(r11 + 24, f10_3);
  const float f27_6 = (float)f27_5;
  stf(r11 + 28, f13_1);
  r11 = r11 + 32;
  const double f24_1 = -((double)f0_1 * f9_6 + -(double)f7_1);
  r10 = (u32)-2113929216;
  const double f23_1 = -((double)f0_1 * f8_7 + -(double)f6_1);
  stf(r11 + 0, f27_6);
  const float f28_8 = (float)f28_7;
  stf(r11 + 4, f28_8);
  stf(r11 + 8, f2_1);
  const float f26_4 = (float)f26_3;
  stf(r11 + 12, f13_1);
  const float f25_2 = (float)f25_1;
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  const float f28_9 = (float)f24_1;
  stf(r11 + 28, f13_1);
  const float f27_7 = (float)f23_1;
  stf(r11 + 32, f26_4);
  const double f24_2 = ((double)f0_1 * f8_7 + (double)f6_1);
  stf(r11 + 36, f25_2);
  const double f25_3 = ((double)f0_1 * f9_6 + (double)f7_1);
  stf(r11 + 40, f2_1);
  const double f23_2 = -((double)f0_1 * f29_2 + -(double)f3_1);
  stf(r11 + 44, f1_1);
  stf(r11 + 48, f12_2);
  stf(r11 + 52, f11_3);
  stf(r11 + 56, f10_3);
  stf(r11 + 60, f13_1);
  r11 = r3 + 32;
  stf(r3 + 0, f28_9);
  const double f28_10 = ((double)f0_1 * f9_6 + (double)f7_1);
  stf(r3 + 4, f27_7);
  const double f27_8 = ((double)f0_1 * f8_7 + (double)f6_1);
  stf(r3 + 8, f3_1);
  const double f24_3 = -((double)f0_1 * f30_2 + -f24_2);
  stf(r3 + 12, f1_1);
  const double f25_4 = -((double)f0_1 * f31_2 + -f25_3);
  stf(r3 + 16, f12_2);
  const double f7_2 = -((double)f0_1 * f9_6 + -(double)f7_1);
  stf(r3 + 20, f11_3);
  stf(r3 + 24, f10_3);
  stf(r3 + 28, f13_1);
  const float f26_5 = (float)f28_10;
  stf(r11 + 0, f26_5);
  const float f27_9 = (float)f27_8;
  stf(r11 + 4, f27_9);
  stf(r11 + 8, f3_1);
  const float f26_6 = (float)f24_3;
  stf(r11 + 12, f13_1);
  const float f25_5 = (float)f25_4;
  stf(r11 + 16, f12_2);
  const float f27_10 = (float)f23_2;
  stf(r11 + 20, f11_3);
  const double f7_3 = -((double)f0_1 * f31_2 + -f7_2);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f13_1);
  r11 = r11 + 32;
  const float f28_11 = ldf(r10 + 1740);
  stf(r11 + 0, f25_5);
  stf(r11 + 4, f26_6);
  stf(r11 + 8, f27_10);
  stf(r11 + 12, f13_1);
  const float f7_4 = (float)f7_3;
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f28_11);
  r11 = r11 + 32;
  stf(r11 + 0, f7_4);
  const double f7_5 = -((double)f0_1 * f8_7 + -(double)f6_1);
  const double f6_2 = -((double)f0_1 * f29_2 + -(double)f3_1);
  const double f7_6 = -((double)f0_1 * f30_2 + -f7_5);
  const float f7_7 = (float)f7_6;
  stf(r11 + 4, f7_7);
  const float f6_3 = (float)f6_2;
  stf(r11 + 8, f6_3);
  const double f6_4 = -((double)f0_1 * f9_6 + -(double)f5_1);
  stf(r11 + 12, f1_1);
  stf(r11 + 16, f12_2);
  const double f3_2 = -((double)f0_1 * f8_7 + -(double)f4_1);
  stf(r11 + 20, f11_3);
  const double f27_11 = ((double)f0_1 * f9_6 + (double)f5_1);
  stf(r11 + 24, f10_3);
  const double f26_7 = ((double)f0_1 * f8_7 + (double)f4_1);
  stf(r11 + 28, f28_11);
  r11 = r11 + 32;
  const double f7_8 = f9_6 + f31_2;
  const float f6_5 = (float)f6_4;
  stf(r11 + 0, f6_5);
  const float f3_3 = (float)f3_2;
  stf(r11 + 4, f3_3);
  stf(r11 + 8, f2_1);
  const float f27_12 = (float)f27_11;
  stf(r11 + 12, f1_1);
  const float f26_8 = (float)f26_7;
  stf(r11 + 16, f12_2);
  const double f6_6 = f8_7 + f30_2;
  stf(r11 + 20, f11_3);
  const double f7_9 = (f7_8 * (double)f0_1 + (double)f5_1);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f13_1);
  r11 = r11 + 32;
  stf(r11 + 0, f27_12);
  stf(r11 + 4, f26_8);
  stf(r11 + 8, f2_1);
  stf(r11 + 12, f13_1);
  const float f7_10 = (float)f7_9;
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f13_1);
  r11 = r11 + 32;
  stf(r11 + 0, f7_10);
  const double f7_11 = (f6_6 * (double)f0_1 + (double)f4_1);
  const double f6_7 = ((double)f0_1 * f29_2 + (double)f2_1);
  const float f7_12 = (float)f7_11;
  stf(r11 + 4, f7_12);
  const float f7_13 = (float)f6_7;
  stf(r11 + 8, f7_13);
  stf(r11 + 12, f13_1);
  const double f13_2 = -((double)f0_1 * f9_6 + -(double)f5_1);
  const double f9_7 = -((double)f0_1 * f8_7 + -(double)f4_1);
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f28_11);
  const double f13_3 = ((double)f0_1 * f31_2 + f13_2);
  const double f9_8 = ((double)f0_1 * f30_2 + f9_7);
  const double f0_2 = ((double)f0_1 * f29_2 + (double)f2_1);
  const float f13_4 = (float)f13_3;
  stf(r11 + 32, f13_4);
  const float f9_9 = (float)f9_8;
  stf(r11 + 36, f9_9);
  const float f0_3 = (float)f0_2;
  stf(r11 + 40, f0_3);
  stf(r11 + 44, f1_1);
  stf(r11 + 48, f12_2);
  stf(r11 + 52, f11_3);
  stf(r11 + 56, f10_3);
  stf(r11 + 60, f28_11);
  (void)r10;
  ctx.r1.u64 = sp;
  ctx.lr = 0x82061078;  // as the original leaves it (its register-restore call)
}

// ---------------------------------------------------------------------------
// A glow sprite (guest 0x8205FA98): r3 its centre (x, y), f1 its half size,
// r5 its colour (r, g, b; times 1.5). Four vertices from the stream at
// 0x821A6988 (the game's allocator 0x820617E8, guest), 7 floats each
// (x, y, u, v, r, g, b), corners (-,-) (+,-) (+,+) (-,+), written big-endian
// into the game's vertex chunk as the original does.
extern "C" void sub_820617E8(PPCContext& ctx, uint8_t* base);  // vertices from a dynamic stream (guest)
extern "C" void sub_8205FA98(PPCContext& ctx, uint8_t* base) {
  const u32 centre = ctx.r3.u32, colour = ctx.r5.u32;
  const double size = ctx.f1.f64;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32;
  gw_store32(sp - 112, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = sp - 112;
  // (Everything is read after the allocation, as the original.)
  ctx.r3.u64 = 0x821A6988;
  ctx.r4.u64 = 4;
  ctx.lr = 0x8205FACC;
  sub_820617E8(ctx, base);
  const u32 v = ctx.r3.u32;
  const float c0 = ldf(colour + 0), c1 = ldf(colour + 4), x = ldf(centre + 0), c2 = ldf(colour + 8);
  const float y = ldf(centre + 4);
  const float zero = ldf(kZero), k = ldf(C + 3216), one = ldf(kOne);
  const float x0 = (float)((double)x - size), x1 = (float)((double)x + size);
  const float y0 = (float)((double)y - size), y1 = (float)((double)y + size);
  const float r = c0 * k, g = c1 * k, b = c2 * k;
  const float corner[4][4] = {{x0, y0, zero, zero}, {x1, y0, one, zero}, {x1, y1, one, one}, {x0, y1, zero, one}};
  for (u32 i = 0; i < 4; ++i) {
    const u32 p = v + i * 28;
    for (u32 j = 0; j < 4; ++j) stf(p + j * 4, corner[i][j]);
    stf(p + 16, r);
    stf(p + 20, g);
    stf(p + 24, b);
  }
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// An entity's glow (guest 0x82045B18, r3 the entity): through its handle
// (+120 object, +124 generation, +126 offset; nothing when dead) its owner's
// colour (+44/+48/+52) times its alpha (+56); while the entity fades (flag
// 0x10) times 0.5 (1 + f(3 x its timer +56, 1.0)) by the game's CRT routine
// 0x8212FC00 (guest); then a glow sprite (0x8205FA98, above) at the entity's
// position (+4/+8) with the owner's +60 as its size. The original passes the
// colour and position through its frame; written there as it does.
extern "C" void sub_8212FC00(PPCContext& ctx, uint8_t* base);  // CRT (guest)
extern "C" void sub_82045B18(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  // The handle's target, or 0 (the original then reads from 0 + offset).
  auto owner = [&]() -> u32 {
    const u32 h = gw_load32(self + 120);
    if (gw_load16(h) != gw_load16(self + 124)) return 0;
    const u32 p = gw_load32(h + 4);
    if (!p) return 0;
    return p + gw_load16(self + 126);
  };
  if (!owner()) return;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 144;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  const float one = ldf(kOne);
  const u32 colour = frame + 80, pos = frame + 96;
  for (u32 k = 0; k < 16; k += 4) stf(colour + k, one);
  if (gw_load32(self + 36) & 0x10) {
    ctx.f1.f64 = (double)(ldf(self + 56) * ldf(C + 1784));
    ctx.f2.f64 = ldd(C + 3192);
    ctx.lr = 0x82045BA4;
    sub_8212FC00(ctx, base);
    const float f = (float)ctx.f1.f64;
    const float scale = (float)((double)f * (double)ldf(kHalf) + (double)one);
    const u32 p = owner();
    const float a = ldf(p + 56);
    const float r = a * ldf(p + 44), g = ldf(p + 48) * a, b = ldf(p + 52) * a;
    stf(colour + 0, r * scale);
    stf(colour + 4, g * scale);
    stf(colour + 8, b * scale);
  } else {
    const u32 p = owner();
    const float a = ldf(p + 56);
    stf(colour + 0, a * ldf(p + 44));
    stf(colour + 4, ldf(p + 48) * a);
    stf(colour + 8, ldf(p + 52) * a);
  }
  stf(colour + 12, one);
  const u32 p = owner();
  stf(pos + 0, ldf(self + 4));
  stf(pos + 4, ldf(self + 8));
  stf(pos + 12, one);
  stf(pos + 8, ldf(kZero));
  ctx.r3.u64 = pos;
  ctx.r5.u64 = colour;
  ctx.f1.f64 = (double)ldf(p + 60);
  ctx.lr = 0x82045CD4;
  sub_8205FA98(ctx, base);
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// A four-way roller's update (guest 0x82044330, r3 the enemy): it moves
// along one of four directions (+164/+168), flipping over as it goes.
//  - colour from 0x8219E710; its phase +128 runs with the time step; each
//    period (0x8219E788) it picks a new direction (+172) other than the
//    current one: the game's random integer mod 6, where 4 and 5 mean toward
//    the nearest player (0x8204F458) along its larger axis (a random one of
//    four without a player); the flip targets (+144 by +-pi, +156 by pi, the
//    byte +176 toggled) change with it, and sound 58 plays (0x820787F8);
//  - its angle +20 eases toward +132 (spin +136); +140 and +152 approach
//    +144 and +156 by at most pi a second (their steps in +148, +160);
//  - its velocity (+12/+16) loses 40 |sin(pi phase)| along the direction
//    before (the phase as it was) and gains 40 |sin(pi phase)| after
//    (the new phase), damped by 0.9 in between; its radius +32 is
//    7 - 2 |sin +140 + sin +152|; separation (strength 1) in between.
// Guest calls: the random integer (0x82030058), the sound, the sine
// (src/math_native.cpp), separation (src/entity_native.cpp).
extern "C" void sub_820787F8(PPCContext& ctx, uint8_t* base);  // play a sound (guest)
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);  // sine (src/math_native.cpp)
extern "C" void sub_82044330(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;  // (ctx.lr left at the last call's return, as the original)
  const u32 sp = ctx.r1.u32, frame = sp - 160;
  ctx.lr = 0x82044338;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  for (u32 k = 0; k < 16; k += 4) gw_store32(self + 40 + k, gw_load32(0x8219E6F0 + 32 + k));
  const float pi = ldf(C + 6060);
  const float phase_before = ldf(self + 128);
  const float phase = phase_before + ldf(kDeltaTime);
  stf(self + 128, phase);
  const float period = ldf(0x8219E768 + 32);
  const float one = ldf(kOne);
  if (!(phase < period)) {
    stf(self + 128, phase - period);
    const float zero = ldf(kZero);
    u32 dir;
    for (;;) {
      ctx.r3.u64 = 0x821A1E28;
      ctx.lr = 0x820443D0;
      sub_82030058(ctx, base);
      dir = ctx.r3.u32 % 6;
      if (dir >= 4) {
        ctx.r3.u64 = self;
        ctx.lr = 0x820443F8;
        sub_8204F458(ctx, base);
        const u32 player = ctx.r3.u32;
        if (player) {
          const float dx = ldf(player + 4) - ldf(self + 4);
          const float dy = ldf(player + 8) - ldf(self + 8);
          if (__builtin_fabsf(dx) > __builtin_fabsf(dy)) dir = dx < zero ? 0 : 1;
          else dir = dy < zero ? 2 : 3;
        } else {
          ctx.r3.u64 = 0x821A1E28;
          ctx.lr = 0x82044460;
          sub_82030058(ctx, base);
          dir = ctx.r3.u32 & 3;
        }
      }
      if ((i32)dir != (i32)gw_load32(self + 172)) break;
    }
    const float minus_one = ldf(C + 3208);
    float ax = zero, ay = zero;  // the new direction (+164, +168)
    const bool flipped = gw_load8(self + 176) != 0;
    switch (dir) {
      case 0:
        gw_store32(self + 172, 1);
        ax = minus_one;
        stf(self + 144, (float)((double)(flipped ? one : minus_one) * (double)pi + (double)ldf(self + 144)));
        break;
      case 1:
        gw_store32(self + 172, 0);
        ax = one;
        stf(self + 144, (float)(-((double)(flipped ? one : minus_one) * (double)pi - (double)ldf(self + 144))));
        break;
      case 2: {
        const float t = ldf(self + 156) + pi;
        ay = minus_one;
        gw_store8(self + 176, flipped ? 0 : 1);
        gw_store32(self + 172, 3);
        stf(self + 156, t);
        break;
      }
      default: {
        ay = one;
        gw_store32(self + 172, 2);
        gw_store8(self + 176, flipped ? 0 : 1);
        stf(self + 156, ldf(self + 156) - pi);
        break;
      }
    }
    ctx.r3.u64 = 58;
    stf(self + 164, ax);
    stf(self + 168, ay);
    ctx.lr = 0x820445D8;
    sub_820787F8(ctx, base);
  }
  {
    const float angle = ldf(self + 20);
    const float spin = ldf(self + 136) * ldf(C + 1876);
    const float gap = ldf(self + 132) - angle;
    const float s = (float)((double)gap * (double)ldf(C + 3420) + (double)spin);
    stf(self + 136, s);
    stf(self + 20, s + angle);
  }
  const float minus_pi = ldf(C + 4344);
  // +140 toward +144, then +152 toward +156, at most pi a second each.
  for (u32 at = self + 140; at <= self + 152; at += 12) {
    const float dt = ldf(kDeltaTime);
    const float cur = ldf(at);
    const float up = dt * pi, down = dt * minus_pi;
    const float d = ldf(at + 4) - cur;
    const float lo = down - d, hi = d - up;
    float v = lo >= 0 ? down : d;
    v = hi >= 0 ? up : v;
    stf(at + 8, v);
    stf(at, v + cur);
  }
  const u32 vel = self + 12;
  const float k40 = ldf(C + 4188);
  ctx.f1.f64 = (double)(phase_before * pi);
  ctx.lr = 0x82044690;
  sub_8212E9B0(ctx, base);
  {
    const float a = __builtin_fabsf((float)ctx.f1.f64);
    const float vy = ldf(vel + 4);
    const float px = (ldf(self + 164) * a) * k40, py = (ldf(self + 168) * a) * k40;
    const float wx = ldf(vel + 0) - px;
    stf(vel + 0, wx);
    const float wy = vy - py;
    stf(vel + 4, wy);
    const float damp = ldf(C + 3440);
    stf(vel + 0, wx * damp);
    stf(vel + 4, wy * damp);
  }
  ctx.f1.f64 = (double)ldf(self + 140);
  ctx.lr = 0x820446F8;
  sub_8212E9B0(ctx, base);
  const float s1 = (float)ctx.f1.f64;
  ctx.f1.f64 = (double)ldf(self + 152);
  ctx.lr = 0x82044708;
  sub_8212E9B0(ctx, base);
  {
    const float s2 = (float)ctx.f1.f64;
    const float m = __builtin_fabsf(s2 + s1);
    const float radius = (float)(-((double)m * (double)ldf(C + 1788) - (double)ldf(C + 6064)));
    stf(self + 32, radius);
    ctx.r3.u64 = self;
    ctx.f1.f64 = (double)radius;
    ctx.f2.f64 = (double)one;
    ctx.lr = 0x82044738;
    sub_82046248(ctx, base);
  }
  ctx.f1.f64 = (double)(ldf(self + 128) * pi);
  ctx.lr = 0x82044748;
  sub_8212E9B0(ctx, base);
  {
    const float a = __builtin_fabsf((float)ctx.f1.f64);
    const float px = (ldf(self + 164) * a) * k40, py = (ldf(self + 168) * a) * k40;
    stf(vel + 0, px + ldf(vel + 0));
    stf(vel + 4, ldf(vel + 4) + py);
  }
  ctx.r1.u64 = sp;
}

// ---------------------------------------------------------------------------
// A bouncing drifter's update (guest 0x82054E30, r3 the enemy). Its colour
// from 0x8219FE78. Spinning (byte +136): it turns by its remaining spin
// +140 times 0x8219FE10, at most 0x8219FE44 a frame, the angle +20 wrapped
// to +-pi, its velocity (+12/+16) damped by 0x8219FDA8; under 0.01 left the
// spin is finished. Else it accelerates along its heading +132 at its speed
// +128 (by 0x8219FD40, damped by 0x8219FD74) and probes ahead (0x8219FDDC
// of the step) against the arena (0x8205C5D8, its probe and push in the
// frame as the original); a push facing it by more than -0.5 reverses its
// speed and starts a half turn of +-pi (by the game's random float
// 0x82030430, guest) with sound 57. Always: +144 grows by 0.1, wrapped to
// +-pi. Guest calls: sine and cosine (src/math_native.cpp), the random
// float, the sound.
extern "C" void sub_82030430(PPCContext& ctx, uint8_t* base);  // random float (guest)
namespace {

// fctid as the recompiled code does it (round to nearest), then fcfid.
inline double round_to_integer(double x) {
  const i64 n = __builtin_isnan(x)              ? (i64)0x8000000000000000ULL
                : (x > (double)LLONG_MAX)       ? LLONG_MAX
                                                : simde_mm_cvtsd_si64(simde_mm_load_sd(&x));
  return (double)n;
}

// An angle wrapped to +-pi as the game does: a - 2 pi round(a / 2 pi).
inline float wrap_angle(float a) {
  const double turns = (double)a * ldd(C + 3328);
  return (float)((turns - round_to_integer(turns)) * ldd(C + 3320));
}

}  // namespace

extern "C" void sub_82054E30(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 144;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  for (u32 k = 0; k < 16; k += 4) gw_store32(self + 40 + k, gw_load32(0x8219FE58 + 32 + k));
  const u32 vel = self + 12;
  if (gw_load8(self + 136)) {
    const float left = ldf(self + 140), angle = ldf(self + 20);
    const float most = ldf(0x8219FE24 + 32);
    const float step0 = left * ldf(0x8219FDF0 + 32);
    const float neg = -most;
    const float lo = neg - step0, hi = step0 - most;
    float step = lo >= 0 ? neg : step0;
    step = hi >= 0 ? most : step;
    const float turned = step + angle;
    stf(self + 140, left - step);
    stf(self + 20, wrap_angle(turned));
    const float damp = ldf(0x8219FD88 + 32);
    const float vx = ldf(vel + 0) * damp, vy = ldf(vel + 4) * damp;
    stf(vel + 0, vx);
    stf(vel + 4, vy);
    const float rest = ldf(self + 140);
    if (__builtin_fabsf(rest) < ldf(C + 4528)) {
      stf(self + 20, rest + ldf(self + 20));
      stf(self + 140, ldf(kZero));
      gw_store8(self + 136, 0);
    }
  } else {
    ctx.f1.f64 = (double)ldf(self + 132);
    ctx.lr = 0x82054F60;
    sub_8212E9B0(ctx, base);
    const float s = (float)ctx.f1.f64;
    ctx.f1.f64 = (double)ldf(self + 132);
    ctx.lr = 0x82054F70;
    sub_8212EA80(ctx, base);
    const float c = (float)ctx.f1.f64;
    const float speed = ldf(self + 128);
    const u32 push = frame + 80, probe = frame + 88;
    const float zero = ldf(kZero);
    stf(push + 0, zero);
    stf(push + 4, zero);
    const float sy = speed * s, sx = speed * c;
    {
      const float vx = ldf(vel + 0), vy = ldf(vel + 4);
      const float accel = ldf(0x8219FD20 + 32);
      stf(vel + 4, vy + sy * accel);
      stf(vel + 0, vx + sx * accel);
      const float damp = ldf(0x8219FD54 + 32);
      const float wx = damp * ldf(vel + 0), wy = ldf(vel + 4) * damp;
      stf(vel + 0, wx);
      stf(vel + 4, wy);
    }
    const float ahead = ldf(0x8219FDBC + 32);
    const float px = sx * ahead, py = sy * ahead;
    stf(probe + 0, px + ldf(self + 4));
    stf(probe + 4, ldf(self + 8) + py);
    ctx.lr = 0x82055020;
    if (keep_in_arena(probe, (double)ldf(self + 32), push)) {
      ctx.f1.f64 = (double)ldf(self + 132);
      const float speed2 = ldf(self + 128);
      ctx.lr = 0x82055038;
      sub_8212E9B0(ctx, base);
      const float s2 = (float)ctx.f1.f64;
      ctx.f1.f64 = (double)ldf(self + 132);
      ctx.lr = 0x82055048;
      sub_8212EA80(ctx, base);
      const float c2 = (float)ctx.f1.f64;
      const float facing = (s2 * speed2) * ldf(push + 4) + (c2 * speed2) * ldf(push + 0);
      if (facing < ldf(C + 4656)) {
        const float sp2 = ldf(self + 128);
        gw_store8(self + 136, 1);
        stf(self + 128, sp2 * ldf(C + 3208));
        ctx.r3.u64 = 0x821A1E28;
        ctx.f1.f64 = (double)ldf(kOne);
        ctx.lr = 0x820550A8;
        sub_82030430(ctx, base);
        const float turn = ctx.f1.f64 > (double)ldf(kHalf) ? ldf(C + 8488) : ldf(C + 4344);
        ctx.r3.u64 = 57;
        stf(self + 140, turn);
        ctx.lr = 0x820550D8;
        sub_820787F8(ctx, base);
      }
    }
  }
  stf(self + 144, wrap_angle(ldf(self + 144) + ldf(C + 1812)));
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}


// ---------------------------------------------------------------------------
// The four-way roller's world matrix (guest 0x82044168, a virtual): r3 the
// 4 x 4 out, r4 the roller. Its two flip angles (+152 about x, +140 about
// y) as rotations (0x82041600, src/math_native.cpp, into the frame as the
// original), their product (y rotation times x rotation, as
// 0x82041CA0 computes it, here in registers), placed at its position
// (+4/+8) raised by 8 |sin +140 + sin +152|. Returns r3.
extern "C" void sub_82044168(PPCContext& ctx, uint8_t* base) {
  const u32 out = ctx.r3.u32, ent = ctx.r4.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 368;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  ctx.f1.f64 = (double)ldf(ent + 140);
  ctx.lr = 0x82044194;
  sub_8212E9B0(ctx, base);
  const float s140 = (float)ctx.f1.f64;
  ctx.f1.f64 = (double)ldf(ent + 152);
  ctx.lr = 0x820441A4;
  sub_8212E9B0(ctx, base);
  const float s152 = (float)ctx.f1.f64;
  const float a152 = ldf(ent + 152), x = ldf(ent + 4), y = ldf(ent + 8), a140 = ldf(ent + 140);
  const float one = ldf(kOne), zero = ldf(kZero);
  const u32 axis_x = frame + 80, axis_y = frame + 96, rot_x = frame + 112, rot_y = frame + 176;
  stf(axis_x + 0, one);
  stf(axis_y + 4, one);
  stf(axis_x + 4, zero);
  stf(axis_x + 8, zero);
  stf(axis_x + 12, zero);
  stf(axis_y + 0, zero);
  stf(axis_y + 8, zero);
  stf(axis_y + 12, zero);
  const float lift = __builtin_fabsf(s152 + s140) * ldf(C + 3352);
  ctx.r3.u64 = rot_x;
  ctx.r4.u64 = axis_x;
  ctx.f1.f64 = (double)a152;
  ctx.lr = 0x8204420C;
  sub_82041600(ctx, base);
  ctx.r3.u64 = rot_y;
  ctx.r4.u64 = axis_y;
  ctx.f1.f64 = (double)a140;
  ctx.lr = 0x82044220;
  sub_82041600(ctx, base);
  float b[16], m[16];
  for (u32 i = 0; i < 16; ++i) b[i] = ldf(rot_x + i * 4), m[i] = ldf(rot_y + i * 4);
  float p[16];
  for (u32 row = 0; row < 4; ++row) {
    const float v0 = m[row * 4], v1 = m[row * 4 + 1], v2 = m[row * 4 + 2], v3 = m[row * 4 + 3];
    for (u32 c = 0; c < 4; ++c) {
      float t = v0 * b[c] + v1 * b[4 + c];
      t = t + v2 * b[8 + c];
      p[row * 4 + c] = t + v3 * b[12 + c];
    }
  }
  for (u32 i = 0; i < 12; ++i) stf(out + i * 4, p[i]);
  stf(out + 48, p[12] + x);
  stf(out + 52, p[13] + y);
  stf(out + 56, p[14] + lift);
  stf(out + 60, p[15]);
  ctx.r3.u64 = out;
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// Line debris (guest 0x82074B28): the pieces a destroyed enemy breaks into,
// r3 the set ({+0 the pieces, +4 how many}; 92 bytes a piece). Each piece's
// life (+68) runs down by the time step; at zero it is removed: the last
// piece is copied over it (the game's 0x82074538, here a word copy in its
// order) and, as in the original, the copied piece waits for the next frame.
// A live piece's two ends (+0/+4 and +8/+12, with velocities and their
// spin) move, bounce off the screen's edges (D+9652, D+9656) and are pulled
// together by a spring toward its length +64 (strengths at 0x821B470C and
// 0x821B4740). The arithmetic follows the original instruction by
// instruction (fN_k: the k-th value of its register fN; single steps in
// float, fsel and double steps in double, the stack slots it spills to as
// values), which the checker confirms bit for bit.
extern "C" void sub_82074B28(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 set = ctx.r3.u32;
  if (set) {
    const u32 count = gw_load32(set + 4), first = gw_load32(set + 0);
    u32 r25 = count * 92 + first, removed = 0;
    if (first < r25) {
      const float zero = ldf(kZero), one = ldf(kOne), half = ldf(kHalf);
      const double k31 = ldd(C + 10856), k27 = ldd(C + 10112);
      const u32 r26 = 0x82180000, r27 = 0x82180000, r28 = 0x821B0000 + 18208, r29 = 0x821B0000 + 18156;
      const u32 r30 = 0x821A0000;
      for (u32 r31 = first + 24; r31 - 24 < r25; r31 += 92) {
        const float life = ldf(r31 + 44) - ldf(kDeltaTime);
        stf(r31 + 44, life);
        if (!(life > zero)) {
          r25 -= 92;
          for (u32 k = 0; k < 92; k += 4) gw_store32(r31 - 24 + k, gw_load32(r25 + k));
          ++removed;
          continue;
        }
      const float f0_1 = ldf(r30 + -11648);
      const float f6_1 = ldf(r31 + -8);
      const float f5_1 = ldf(r31 + -4);
      const float f11_1 = f0_1 * f6_1;
      const float f10_1 = f0_1 * f5_1;
      const float f24_1 = ldf(r31 + 0);
      const float f23_1 = ldf(r31 + 4);
      const float f9_1 = f24_1 * f0_1;
      const float f22_1 = ldf(r31 + 32);
      const float f2_1 = f23_1 * f0_1;
      const float f4_1 = ldf(r31 + 24);
      const float f28_1 = f22_1 * f0_1;
      const float f3_1 = ldf(r31 + 28);
      const float f1_1 = f4_1 * f0_1;
      const float f21_1 = ldf(r31 + 36);
      const float f29_1 = f3_1 * f0_1;
      const float f0_2 = f21_1 * f0_1;
      const float f8_1 = ldf(r31 + -24);
      const float f7_1 = ldf(r31 + -20);
      const float f15_1 = ldf(r31 + 20);
      const float f8_2 = f11_1 + f8_1;
      const float f20_1 = ldf(r31 + -16);
      const float f7_2 = f10_1 + f7_1;
      const float f13_1 = ldf(r27 + 9652);
      const float f12_1 = ldf(r26 + 9656);
      const float f20_2 = f20_1 + f9_1;
      const float f19_1 = ldf(r31 + -12);
      const float f26_1 = -f13_1;
      const float f18_1 = ldf(r31 + 8);
      const float f19_2 = f19_1 + f2_1;
      const float f17_1 = ldf(r31 + 12);
      const float f2_2 = f18_1 + f1_1;
      const float f16_1 = ldf(r31 + 16);
      const float f1_2 = f29_1 + f17_1;
      const float f0_3 = f0_2 + f15_1;
      const float f18_2 = f16_1 + f28_1;
      const float f25_1 = -f12_1;
      const float f10_2 = f13_1 - f8_2;
      const float f9_2 = f12_1 - f7_2;
      const float f0_4 = f13_1 + f8_2;
      const float f11_2 = f12_1 + f7_2;
      const double f15_2 = (double)f13_1 + (double)f10_2;
      const double f16_2 = f9_2 >= 0.0 ? k27 : k31;
      const float f28_2 = f0_4 >= 0.0f ? f10_2 : f0_4;
      const float f17_2 = f11_2 >= 0.0f ? f9_2 : f11_2;
      const double f29_2 = (double)f26_1 - (double)f0_4;
      const double f28_3 = (double)f25_1 - (double)f11_2;
      const double f17_3 = f10_2 >= 0.0 ? k27 : k31;
      const double f14_1 = (double)f12_1 + (double)f9_2;
      const double f8_3 = f0_4 >= 0.0 ? (double)f8_2 : f29_2;
      const double f7_3 = f11_2 >= 0.0 ? (double)f7_2 : f28_3;
      const double f11_3 = f11_2 >= 0.0 ? f16_2 : k31;
      const double f0_5 = f0_4 >= 0.0 ? f17_3 : k31;
      const float f29_3 = f18_2 - f20_2;
      const double f10_3 = f10_2 >= 0.0 ? f8_3 : f15_2;
      const double f9_3 = f9_2 >= 0.0 ? f7_3 : f14_1;
      const double f14_2 = f11_3 * (double)f5_1;
      const double f0_6 = f0_5 * (double)f6_1;
      const float f8_4 = f12_1 - f1_2;
      const float f7_4 = f13_1 - f2_2;
      const float f17_4 = (float)f10_3;
      const float f16_3 = (float)f9_3;
      const float f9_4 = f13_1 + f2_2;
      const float f28_4 = f0_3 - f19_2;
      const float f15_3 = f28_2 >= 0.0f ? f17_2 : f28_2;
      const float f10_4 = ldf(r31 + 40);
      const float f11_4 = f1_2 + f12_1;
      const double f6_2 = f15_3 >= 0.0 ? (double)f6_1 : f0_6;
      const float f0_7 = ldf(r28 + 32);
      const float f24_2 = f24_1 * f0_7;
      const float f23_2 = f23_1 * f0_7;
      const float f22_2 = f22_1 * f0_7;
      const float f21_2 = f21_1 * f0_7;
      const double f5_2 = f15_3 >= 0.0 ? (double)f5_1 : f14_2;
      const float f6_3 = (float)f6_2;
      const float f5_3 = (float)f5_2;
      const double f5_4 = (double)f25_1 - (double)f11_4;
      const double f25_2 = (double)f12_1 + (double)f8_4;
      const float f12_2 = f11_4 >= 0.0f ? f8_4 : f11_4;
      const double f12_3 = f8_4 >= 0.0 ? k27 : k31;
      const double f26_2 = (double)f26_1 - (double)f9_4;
      const double f14_3 = f7_4 >= 0.0 ? k27 : k31;
      const double f15_4 = (double)f13_1 + (double)f7_4;
      const float f13_2 = f9_4 >= 0.0f ? f7_4 : f9_4;
      const float f12_4 = f29_3 * f29_3;
      const double f1_3 = f11_4 >= 0.0 ? (double)f1_2 : f5_4;
      const float f5_5 = f28_4 * f28_4;
      const double f2_3 = f9_4 >= 0.0 ? (double)f2_2 : f26_2;
      const double f9_5 = f9_4 >= 0.0 ? f14_3 : k31;
      const double f1_4 = f8_4 >= 0.0 ? f1_3 : f25_2;
      const float f25_3 = f5_5 + f12_4;
      const double f9_6 = f9_5 * (double)f4_1;
      const double f11_5 = f11_4 >= 0.0 ? f12_3 : k31;
      const double f26_3 = f7_4 >= 0.0 ? f2_3 : f15_4;
      const float f7_5 = (float)f1_4;
      const float f2_4 = f0_7 * f6_3;
      const double f8_5 = f11_5 * (double)f3_1;
      const float f1_5 = f0_7 * f5_3;
      const float f11_6 = f13_2 >= 0.0f ? f12_2 : f13_2;
      const float f6_4 = (float)f26_3;
      const float f12_5 = f7_5 - f16_3;
      const float f7_6 = f7_5 + f16_3;
      const double f9_7 = f11_6 >= 0.0 ? (double)f4_1 : f9_6;
      const float f4_2 = ldf(r29 + 32);
      const double f8_6 = f11_6 >= 0.0 ? (double)f3_1 : f8_5;
      const float f13_3 = f6_4 - f17_4;
      const float f11_7 = (float)f9_7;
      const float f9_8 = (float)f8_6;
      const float f8_7 = f12_5 * f12_5;
      const float f5_6 = f13_3 * f13_3;
      const float f3_2 = f11_7 * f0_7;
      const float f26_4 = f9_8 * f0_7;
      const float f0_8 = f25_3 + f8_7;
      const float f0_9 = f0_8 + f5_6;
      const float f11_8 = sqrt_ss(f0_9);
      const float f0_10 = one / f11_8;
      const float f5_7 = f10_4 - f11_8;
      const float f11_9 = f0_10 * f13_3;
      const float f13_4 = f5_7 * f4_2;
      const float f9_9 = f0_10 * f29_3;
      const float f12_6 = f0_10 * f12_5;
      const float f8_8 = f0_10 * f28_4;
      const float f0_11 = f5_7 * f4_2;
      const float f5_8 = f13_4 * f11_9;
      const float f29_4 = f9_9 * f13_4;
      const float f4_3 = f12_6 * f13_4;
      const float f13_5 = f8_8 * f13_4;
      const float f28_5 = f0_11 * f11_9;
      const float f25_4 = f12_6 * f0_11;
      const float f5_9 = f2_4 - f5_8;
      const float f2_5 = f24_2 - f29_4;
      const float f4_4 = f1_5 - f4_3;
      const float f1_6 = f23_2 - f13_5;
      const float f13_6 = f6_4 + f17_4;
      const float f6_5 = f18_2 + f20_2;
      const float f29_5 = f0_3 + f19_2;
      const float f3_3 = f28_5 + f3_2;
      const float f28_6 = f25_4 + f26_4;
      const float f24_3 = f9_9 * f0_11;
      const float f23_3 = f8_8 * f0_11;
      const float f26_5 = f10_4 * f11_9;
      const float f25_5 = f12_6 * f10_4;
      const float f20_3 = f9_9 * f10_4;
      const float f0_12 = f13_6 * half;
      const float f19_3 = f8_8 * f10_4;
      const float f13_7 = f7_6 * half;
      const float f12_7 = f6_5 * half;
      const float f11_10 = f29_5 * half;
      const float f10_5 = f26_5 * half;
      const float f9_10 = f25_5 * half;
      const float f8_9 = f20_3 * half;
      const float f7_7 = f19_3 * half;
      const float f6_6 = f24_3 + f22_2;
      const float f29_6 = f23_3 + f21_2;
      const float f26_6 = f0_12 - f10_5;
      stf(r31 + -24, f26_6);
      const float f25_6 = f13_7 - f9_10;
      stf(r31 + -20, f25_6);
      const float f24_4 = f12_7 - f8_9;
      stf(r31 + -16, f24_4);
      const float f23_4 = f11_10 - f7_7;
      stf(r31 + -12, f23_4);
      stf(r31 + -8, f5_9);
      const float f0_13 = f10_5 + f0_12;
      stf(r31 + -4, f4_4);
      const float f13_8 = f9_10 + f13_7;
      stf(r31 + 0, f2_5);
      const float f12_8 = f8_9 + f12_7;
      stf(r31 + 4, f1_6);
      const float f11_11 = f7_7 + f11_10;
      stf(r31 + 8, f0_13);
      stf(r31 + 12, f13_8);
      stf(r31 + 16, f12_8);
      stf(r31 + 20, f11_11);
      stf(r31 + 24, f3_3);
      stf(r31 + 28, f28_6);
      stf(r31 + 32, f6_6);
      stf(r31 + 36, f29_6);
      }
    }
    gw_store32(set + 4, gw_load32(set + 4) - removed);
  }
  ctx.lr = 0x82074F10;  // as the original leaves it (its register-restore call)
}

// ---------------------------------------------------------------------------
// Entity drawing. The draw dispatch (guest 0x82045740, r3 the entity):
// flag 0x1000000 (+36) first runs the virtual +4; then flag 0x400 hands the
// drawing to the virtual +8; else a fading entity (flags & 0x40010 = 0x10)
// draws twice, pulsing (0x82045628); else its outline: kind 34 (+68) with
// the 0x8205EF18 lines (0x82061A30), others with the 0x8205EB58 lines
// (0x82061908). Outline: r3 the centre (x, y), f1 the angle, r5 the scale
// (x, y), r6 the colour, r7 the segments (pairs of points, 16 bytes a
// pair), r8 how many points; each pair rotated, scaled and placed, then a
// line call with the two points (in the frame, as the original) and
// thickness 1. All four are native here; the lines and the CRT routine
// 0x8212FC00 stay calls (0x8205EB58 is src/shapes_native.cpp's).
extern "C" void sub_8205EB58(PPCContext& ctx, uint8_t* base);  // glowing line (src/shapes_native.cpp)
extern "C" void sub_8205EF18(PPCContext& ctx, uint8_t* base);  // another line style (below)
namespace {

// The outline (0x82061908 / 0x82061A30): lr_* the original's return addresses.
void draw_outline(PPCContext& ctx, uint8_t* base, bool style2) {
  const u32 centre = ctx.r3.u32, scale = ctx.r5.u32, colour = ctx.r6.u32, segs = ctx.r7.u32;
  const i32 count = (i32)ctx.r8.u32;
  const double angle = ctx.f1.f64;
  const u32 sp = ctx.r1.u32, frame = sp - 208;
  ctx.lr = style2 ? 0x82061A40 : 0x82061918;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  if (segs && count >= 0) {
    ctx.f1.f64 = angle;
    ctx.lr = style2 ? 0x82061A70 : 0x82061948;
    sub_8212E9B0(ctx, base);
    const float s = (float)ctx.f1.f64;
    ctx.f1.f64 = angle;
    ctx.lr = style2 ? 0x82061A80 : 0x82061958;
    sub_8212EA80(ctx, base);
    const float c = (float)ctx.f1.f64;
    const float sx = ldf(scale + 0), sy = ldf(scale + 4);
    const float xs = sx * s, ys = sy * s, xc = sx * c, yc = sy * c;
    if (count > 0) {
      const u32 pairs = ((u32)(count - 1) >> 1) + 1;
      const float one = ldf(kOne), zero = ldf(kZero);
      const u32 a = frame + 96, b = frame + 80;
      u32 seg = segs + 8;
      for (u32 n = 0; n < pairs; ++n, seg += 16) {
        const float bx = ldf(seg + 0), ay = ldf(seg - 4), cx = ldf(centre + 0), by = ldf(seg + 4);
        const float ax = ldf(seg - 8), cy = ldf(centre + 4);
        const float t8 = bx * xs, t7 = ay * yc;
        const float u0 = (float)((double)bx * xc + cx);
        const float u10 = (float)((double)ax * xc + cx);
        stf(b + 8, zero);
        stf(b + 12, zero);
        stf(a + 8, zero);
        stf(a + 12, zero);
        const float u8 = (float)((double)by * yc + t8);
        const float u12 = (float)((double)ax * xs + t7);
        stf(b + 0, (float)(-((double)by * ys - u0)));
        stf(b + 4, u8 + cy);
        stf(a + 0, (float)(-((double)ay * ys - u10)));
        stf(a + 4, u12 + cy);
        ctx.r3.u64 = a;
        ctx.r4.u64 = b;
        ctx.r5.u64 = colour;
        ctx.f1.f64 = (double)one;
        if (style2) {
          ctx.lr = 0x82061B38;
          sub_8205EF18(ctx, base);
        } else {
          ctx.lr = 0x82061A10;
          sub_8205EB58(ctx, base);
        }
      }
    }
  }
  ctx.r1.u64 = sp;
  ctx.lr = style2 ? 0x82061B54 : 0x82061A2C;  // as the original leaves it
}

}  // namespace

extern "C" void sub_82061908(PPCContext& ctx, uint8_t* base) { draw_outline(ctx, base, false); }
extern "C" void sub_82061A30(PPCContext& ctx, uint8_t* base) { draw_outline(ctx, base, true); }

// The fading draw (guest 0x82045628, r3 the entity): two outlines, the
// second a step on, each scaled by 0.1 + 0x82000FF8 x its pulse p, where p
// is f(3 x timer +56, 1.0) (the CRT routine 0x8212FC00) plus the pass
// number, folded back past 1 (2 - p); drawn only while the fold is at least
// 0x82000D64, its alpha +52 times the fold, colour +40/+44/+48.
extern "C" void sub_82045628(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const u32 sp = ctx.r1.u32, frame = sp - 208;
  ctx.lr = 0x82045638;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  const float least = ldf(C + 3428), base_k = ldf(C + 1812), step = ldf(C + 4088), two = ldf(C + 1788);
  const float one = ldf(kOne), three = ldf(C + 1784);
  const double unit = ldd(C + 3192);
  const u32 scale = frame + 80, colour = frame + 96;
  for (i32 pass = 0; pass < 2; ++pass) {
    ctx.f1.f64 = (double)(ldf(self + 56) * three);
    ctx.f2.f64 = unit;
    ctx.lr = 0x82045690;
    sub_8212FC00(ctx, base);
    gw_store64(frame + 88, (u64)(i64)pass);  // (the original's conversion slot)
    const float p = (float)ctx.f1.f64 + (float)(double)(i64)pass;
    const float fold = p < one ? p : two - p;
    const float k = (float)((double)p * (double)step + (double)base_k);
    stf(scale + 0, ldf(self + 24) * k);
    stf(scale + 4, ldf(self + 28) * k);
    if (!(fold < least)) {
      stf(colour + 12, ldf(self + 52) * fold);
      stf(colour + 4, ldf(self + 44));
      const float r = ldf(self + 40);
      ctx.r8.u64 = gw_load32(self + 60);
      const float bl = ldf(self + 48);
      ctx.r7.u64 = gw_load32(self + 64);
      ctx.f1.f64 = (double)ldf(self + 20);
      stf(colour + 0, r);
      stf(colour + 8, bl);
      ctx.r3.u64 = self + 4;
      ctx.r5.u64 = scale;
      ctx.r6.u64 = colour;
      ctx.lr = 0x82045724;
      draw_outline(ctx, base, false);
    }
  }
  ctx.r1.u64 = sp;
  ctx.lr = 0x8204573C;  // as the original leaves it
}

extern "C" void sub_82045740(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, frame = sp - 96;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  if (gw_load32(self + 36) & 0x1000000) {
    const u32 fn = gw_load32(gw_load32(self) + 4);
    ctx.lr = 0x82045774;
    REX_CALL_INDIRECT_FUNC(fn);
  }
  const u32 flags = gw_load32(self + 36);
  if (flags & 0x400) {
    ctx.r3.u64 = self;
    const u32 fn = gw_load32(gw_load32(self) + 8);
    ctx.lr = 0x82045798;
    REX_CALL_INDIRECT_FUNC(fn);
  } else if ((flags & 0x40010) == 0x10) {
    ctx.r3.u64 = self;
    ctx.lr = 0x820457C4;
    sub_82045628(ctx, base);
  } else {
    const u32 kind = gw_load32(self + 68);
    ctx.f1.f64 = (double)ldf(self + 20);
    ctx.r8.u64 = gw_load32(self + 60);
    ctx.r6.u64 = self + 40;
    ctx.r7.u64 = gw_load32(self + 64);
    ctx.r5.u64 = self + 24;
    ctx.r3.u64 = self + 4;
    ctx.lr = kind == 34 ? 0x82045800 : 0x82045818;
    draw_outline(ctx, base, kind == 34);
  }
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// A shifting colour (guest 0x82030678): r3 the colour out (4 floats), r4 a
// colour (r, g, b, a), f1 a phase. Six sine/cosine pairs (src/math_native.cpp)
// at steps of the image's angle around the game clock 0x82199260, scaled and
// offset by 0x82199344 / 0x82199378, blended with the colour and normalised
// (the tables at D+1832/1856/1880). Returns r3. The arithmetic follows the
// original instruction by instruction (fN_k: the k-th value of its register
// fN; single steps in float, double steps in double), the calls with the
// registers the original passes, which the checker confirms bit for bit.
extern "C" void sub_82030678(PPCContext& ctx, uint8_t* base) {
  const double angle = ctx.f1.f64;
  const u32 sp = ctx.r1.u32;
  gw_store32(sp - 256, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = sp - 256;
  u32 r11, r29, r30, r31;
  r11 = (u32)-2112356352;
  r31 = (u32)-2112225280;
  r30 = ctx.r3.u32;
  r29 = ctx.r4.u32;
  const float f0_1 = ldf(r11 + 1828);
  r11 = (u32)-2113929216;
  const float f13_1 = ldf(r11 + 1792);
  r11 = (u32)-2112225280;
  const float f29_1 = f0_1 * f13_1;
  r11 = (u32)(r11 + -27868);
  const float f30_1 = ldf(r11 + 32);
  r11 = (u32)-2112225280;
  r11 = (u32)(r11 + -27816);
  const float f28_1 = ldf(r11 + 32);
  r11 = (u32)-2113929216;
  const float f0_2 = ldf(r11 + 1740);
  const float f27_1 = f29_1 * f0_2;
  const float f0_3 = ldf(r31 + -28064);
  const float f0_4 = f27_1 + f0_3;
  const float f1_1 = (float)((double)f0_4 + angle);
  ctx.lr = 0x820306E4;
  ctx.f0.f64 = (double)f0_4;
  ctx.f1.f64 = (double)f1_1;
  ctx.f13.f64 = (double)f13_1;
  sub_8212E9B0(ctx, base);
  const double f1_2 = ctx.f1.f64;
  const float f0_5 = ldf(r31 + -28064);
  const float f0_6 = f27_1 + f0_5;
  const float f27_2 = (float)f1_2;
  const float f0_7 = (float)((double)f0_6 + angle);
  ctx.lr = 0x820306FC;
  ctx.f0.f64 = (double)f0_7;
  ctx.f1.f64 = (double)f0_7;
  sub_8212EA80(ctx, base);
  const double f1_3 = ctx.f1.f64;
  const float f0_8 = ldf(r31 + -28064);
  const float f13_2 = (float)f1_3;
  const float f0_9 = f0_8 + f29_1;
  const float f26_1 = (float)(((double)f27_2 * (double)f30_1 + (double)f28_1));
  const float f25_1 = f13_2 * f30_1;
  const float f0_10 = (float)((double)f0_9 + angle);
  ctx.lr = 0x8203071C;
  ctx.f0.f64 = (double)f0_10;
  ctx.f1.f64 = (double)f0_10;
  ctx.f13.f64 = (double)f13_2;
  sub_8212E9B0(ctx, base);
  const double f1_4 = ctx.f1.f64;
  const float f0_11 = ldf(r31 + -28064);
  const float f24_1 = (float)f1_4;
  const float f0_12 = f0_11 + f29_1;
  const float f0_13 = (float)((double)f0_12 + angle);
  ctx.lr = 0x82030734;
  ctx.f0.f64 = (double)f0_13;
  ctx.f1.f64 = (double)f0_13;
  sub_8212EA80(ctx, base);
  const double f1_5 = ctx.f1.f64;
  r11 = (u32)-2113929216;
  const float f24_2 = (float)(((double)f24_1 * (double)f30_1 + (double)f28_1));
  const float f0_14 = ldf(r11 + 1788);
  const float f27_3 = f29_1 * f0_14;
  const float f0_15 = (float)f1_5;
  const float f23_1 = f0_15 * f30_1;
  const float f0_16 = ldf(r31 + -28064);
  const float f0_17 = f27_3 + f0_16;
  const float f1_6 = (float)((double)f0_17 + angle);
  ctx.lr = 0x8203075C;
  ctx.f0.f64 = (double)f0_17;
  ctx.f1.f64 = (double)f1_6;
  sub_8212E9B0(ctx, base);
  const double f1_7 = ctx.f1.f64;
  const float f0_18 = ldf(r31 + -28064);
  const float f22_1 = (float)f1_7;
  const float f0_19 = f27_3 + f0_18;
  const float f0_20 = (float)((double)f0_19 + angle);
  ctx.lr = 0x82030774;
  ctx.f0.f64 = (double)f0_20;
  ctx.f1.f64 = (double)f0_20;
  sub_8212EA80(ctx, base);
  const double f1_8 = ctx.f1.f64;
  r11 = (u32)-2113929216;
  const float f22_2 = (float)(((double)f22_1 * (double)f30_1 + (double)f28_1));
  const float f0_21 = ldf(r11 + 1784);
  const float f27_4 = f29_1 * f0_21;
  const float f0_22 = (float)f1_8;
  const float f21_1 = f0_22 * f30_1;
  const float f0_23 = ldf(r31 + -28064);
  const float f0_24 = f27_4 + f0_23;
  const float f1_9 = (float)((double)f0_24 + angle);
  ctx.lr = 0x8203079C;
  ctx.f0.f64 = (double)f0_24;
  ctx.f1.f64 = (double)f1_9;
  sub_8212E9B0(ctx, base);
  const double f1_10 = ctx.f1.f64;
  const float f0_25 = ldf(r31 + -28064);
  const float f20_1 = (float)f1_10;
  const float f0_26 = f27_4 + f0_25;
  const float f0_27 = (float)((double)f0_26 + angle);
  ctx.lr = 0x820307B4;
  ctx.f0.f64 = (double)f0_27;
  ctx.f1.f64 = (double)f0_27;
  sub_8212EA80(ctx, base);
  const double f1_11 = ctx.f1.f64;
  r11 = (u32)-2113929216;
  const float f20_2 = (float)(((double)f20_1 * (double)f30_1 + (double)f28_1));
  const float f0_28 = ldf(r11 + 1780);
  const float f27_5 = f29_1 * f0_28;
  const float f0_29 = (float)f1_11;
  const float f19_1 = f0_29 * f30_1;
  const float f0_30 = ldf(r31 + -28064);
  const float f0_31 = f27_5 + f0_30;
  const float f1_12 = (float)((double)f0_31 + angle);
  ctx.lr = 0x820307DC;
  ctx.f0.f64 = (double)f0_31;
  ctx.f1.f64 = (double)f1_12;
  sub_8212E9B0(ctx, base);
  const double f1_13 = ctx.f1.f64;
  const float f0_32 = ldf(r31 + -28064);
  const float f0_33 = f27_5 + f0_32;
  const float f27_6 = (float)f1_13;
  const float f0_34 = (float)((double)f0_33 + angle);
  ctx.lr = 0x820307F4;
  ctx.f0.f64 = (double)f0_34;
  ctx.f1.f64 = (double)f0_34;
  sub_8212EA80(ctx, base);
  const double f1_14 = ctx.f1.f64;
  r11 = (u32)-2113929216;
  const float f27_7 = (float)(((double)f27_6 * (double)f30_1 + (double)f28_1));
  const float f0_35 = ldf(r11 + 1776);
  const float f29_2 = f29_1 * f0_35;
  const float f0_36 = (float)f1_14;
  const float f18_1 = f0_36 * f30_1;
  const float f0_37 = ldf(r31 + -28064);
  const float f0_38 = f29_2 + f0_37;
  const float f1_15 = (float)((double)f0_38 + angle);
  ctx.lr = 0x8203081C;
  ctx.f0.f64 = (double)f0_38;
  ctx.f1.f64 = (double)f1_15;
  sub_8212E9B0(ctx, base);
  const double f1_16 = ctx.f1.f64;
  const float f0_39 = ldf(r31 + -28064);
  const float f0_40 = f29_2 + f0_39;
  const float f0_41 = (float)((double)f0_40 + angle);
  const float f31_1 = (float)f1_16;
  ctx.lr = 0x82030834;
  ctx.f0.f64 = (double)f0_41;
  ctx.f1.f64 = (double)f0_41;
  sub_8212EA80(ctx, base);
  const double f1_17 = ctx.f1.f64;
  r11 = (u32)-2113929216;
  const float f13_3 = ldf(r29 + 12);
  const float f9_1 = (float)f1_17;
  const float f11_1 = ldf(r29 + 4);
  const float f10_1 = ldf(r29 + 0);
  const float f29_3 = (float)(((double)f31_1 * (double)f30_1 + (double)f28_1));
  const float f0_42 = ldf(r29 + 8);
  const float f31_2 = f11_1 - f27_7;
  const float f5_1 = f10_1 - f25_1;
  const float f12_1 = ldf(r11 + 1732);
  const float f28_2 = f0_42 * f0_42;
  const float f13_4 = f13_3 - f12_1;
  const float f8_1 = f11_1 - f24_2;
  const float f7_1 = f11_1 - f22_2;
  const float f4_1 = f10_1 - f23_1;
  const float f3_1 = f10_1 - f21_1;
  const float f30_2 = f9_1 * f30_1;
  const float f9_2 = f11_1 - f26_1;
  const float f26_2 = f0_42 * f0_42;
  const float f6_1 = f11_1 - f20_2;
  const float f24_3 = f0_42 * f0_42;
  const float f27_8 = f13_4 * f13_4;
  const float f25_2 = f13_4 * f13_4;
  const float f23_2 = f13_4 * f13_4;
  const float f22_3 = f0_42 * f0_42;
  const float f21_2 = f13_4 * f13_4;
  const float f8_2 = f8_1 * f8_1;
  const float f9_3 = f9_2 * f9_2;
  const float f7_2 = f7_1 * f7_1;
  const float f6_2 = f6_1 * f6_1;
  const float f2_1 = f10_1 - f19_1;
  const float f28_3 = f27_8 + f28_2;
  const float f27_9 = f25_2 + f26_2;
  const float f26_3 = f23_2 + f24_3;
  const float f5_2 = f5_1 * f5_1;
  const float f25_3 = f21_2 + f22_3;
  const float f4_2 = f4_1 * f4_1;
  const float f3_2 = f3_1 * f3_1;
  const float f1_18 = f10_1 - f18_1;
  const float f11_2 = f11_1 - f29_3;
  const float f2_2 = f2_1 * f2_1;
  const float f9_4 = f28_3 + f9_3;
  const float f8_3 = f27_9 + f8_2;
  const float f7_3 = f26_3 + f7_2;
  const float f10_2 = f10_1 - f30_2;
  const float f6_3 = f25_3 + f6_2;
  const float f1_19 = f1_18 * f1_18;
  const float f9_5 = f9_4 + f5_2;
  const float f8_4 = f8_3 + f4_2;
  const float f7_4 = f7_3 + f3_2;
  const float f5_3 = f0_42 * f0_42;
  const float f6_4 = f6_3 + f2_2;
  const float f9_6 = f12_1 / f9_5;
  const float f8_5 = f12_1 / f8_4;
  const float f7_5 = f12_1 / f7_4;
  const float f6_5 = f12_1 / f6_4;
  const float f4_3 = f13_4 * f13_4;
  r11 = (u32)-2112356352;
  const float f0_43 = f0_42 * f0_42;
  ctx.r10.u64 = (u32)-2112356352;
  const float f13_5 = f13_4 * f13_4;
  ctx.r9.u64 = (u32)-2112356352;
  const float f3_3 = f31_2 * f31_2;
  r11 = (u32)(r11 + 1832);
  const float f11_3 = f11_2 * f11_2;
  ctx.r10.u64 = (u32)(ctx.r10.u32 + 1856);
  const float f10_3 = f10_2 * f10_2;
  ctx.r9.u64 = (u32)(ctx.r9.u32 + 1880);
  ctx.r3.u64 = r30;
  const float f2_3 = ldf(r11 + 0);
  const float f29_4 = ldf(r11 + 4);
  const float f5_4 = f4_3 + f5_3;
  const float f31_3 = ldf(ctx.r10.u32 + 0);
  const float f28_4 = ldf(ctx.r10.u32 + 4);
  const float f0_44 = f13_5 + f0_43;
  const float f30_3 = ldf(ctx.r9.u32 + 0);
  const float f27_10 = ldf(ctx.r9.u32 + 4);
  const float f26_4 = ldf(r11 + 8);
  const float f25_4 = ldf(ctx.r10.u32 + 8);
  const float f24_4 = ldf(ctx.r9.u32 + 8);
  const float f23_3 = ldf(r11 + 12);
  const float f22_4 = ldf(ctx.r10.u32 + 12);
  const float f21_3 = ldf(ctx.r9.u32 + 12);
  const float f5_5 = f5_4 + f3_3;
  const float f20_3 = ldf(r11 + 16);
  const float f19_2 = ldf(ctx.r10.u32 + 16);
  const float f0_45 = f0_44 + f11_3;
  const float f18_2 = ldf(ctx.r9.u32 + 16);
  const float f17_1 = ldf(r11 + 20);
  const float f16_1 = ldf(ctx.r10.u32 + 20);
  const float f15_1 = ldf(ctx.r9.u32 + 20);
  const float f5_6 = f5_5 + f1_19;
  const float f0_46 = f0_45 + f10_3;
  const float f5_7 = f12_1 / f5_6;
  const float f4_4 = f12_1 / f0_46;
  const float f0_47 = f4_4 + f5_7;
  const float f0_48 = f0_47 + f6_5;
  const float f0_49 = f0_48 + f7_5;
  const float f0_50 = f0_49 + f8_5;
  const float f0_51 = f0_50 + f9_6;
  const float f0_52 = f12_1 / f0_51;
  const float f12_2 = f0_52 * f8_5;
  const float f13_6 = f0_52 * f9_6;
  const float f11_4 = f0_52 * f7_5;
  const float f10_4 = f0_52 * f6_5;
  const float f9_7 = f0_52 * f5_7;
  const float f0_53 = f0_52 * f4_4;
  const float f7_6 = f29_4 * f12_2;
  const float f8_6 = f2_3 * f13_6;
  const float f6_6 = f28_4 * f12_2;
  const float f4_5 = f31_3 * f13_6;
  const float f3_4 = f30_3 * f13_6;
  const float f5_8 = f27_10 * f12_2;
  const float f2_4 = f26_4 * f11_4;
  const float f1_20 = f25_4 * f11_4;
  const float f13_7 = f13_6 + f12_2;
  const float f31_4 = f24_4 * f11_4;
  const float f24_5 = f23_3 * f10_4;
  const float f8_7 = f8_6 + f7_6;
  const float f23_4 = f22_4 * f10_4;
  const float f7_7 = f4_5 + f6_6;
  const float f22_5 = f21_3 * f10_4;
  const float f6_7 = f3_4 + f5_8;
  const float f30_4 = f17_1 * f0_53;
  const float f29_5 = f16_1 * f0_53;
  const float f13_8 = f13_7 + f11_4;
  const float f28_5 = f15_1 * f0_53;
  const float f27_11 = f20_3 * f9_7;
  const float f12_3 = f8_7 + f2_4;
  const float f26_5 = f19_2 * f9_7;
  const float f8_8 = f7_7 + f1_20;
  const float f25_5 = f18_2 * f9_7;
  const float f7_8 = f6_7 + f31_4;
  const float f13_9 = f13_8 + f10_4;
  const float f12_4 = f12_3 + f24_5;
  const float f11_5 = f8_8 + f23_4;
  const float f8_9 = f7_8 + f22_5;
  const float f12_5 = f12_4 + f27_11;
  const float f11_6 = f11_5 + f26_5;
  const float f10_5 = f8_9 + f25_5;
  const float f13_10 = f13_9 + f9_7;
  const float f12_6 = f12_5 + f30_4;
  stf(r30 + 0, f12_6);
  const float f12_7 = f11_6 + f29_5;
  stf(r30 + 4, f12_7);
  const float f12_8 = f10_5 + f28_5;
  stf(r30 + 8, f12_8);
  const float f0_54 = f13_10 + f0_53;
  stf(r30 + 12, f0_54);
  ctx.r1.u64 = sp;
  ctx.lr = 0x82030A94;  // as the original leaves it (its register-restore call)
}

// ---------------------------------------------------------------------------
// atan2 (guest 0x8212FE70, the game's CRT): f1 = atan2(f1 = y, f2 = x) in
// double precision, its table at 0x82197DA0 (pi/2 +8, pi +16, 2 - sqrt 3
// +24, sqrt 3 +40, a rational approximation's coefficients +56..+112, the
// offsets 0, pi/6, pi/2, pi/3 at +128). Both zero: y if x is +0, else
// +-pi by y's sign. x zero: +-pi/2. Else the smaller over the larger
// magnitude, folded past 2 - sqrt 3 by (sqrt 3 t - 1) / (sqrt 3 + t), the
// approximation, the octant's offset, mirrored for negative x, signed by y.
// The same double operations in the same order as the original (signs from
// the bits, as its integer tests of the values it spills; the spill itself
// is left out). Leaf; the homing enemy calls it every frame.
extern "C" void sub_8212FE70(PPCContext& ctx, uint8_t* base) {
  (void)base;
  constexpr u32 t = 0x82197DA0;
  const double y = ctx.f1.f64, x = ctx.f2.f64;
  const bool y_negative = (ctx.f1.u64 >> 63) != 0, x_negative = (ctx.f2.u64 >> 63) != 0;
  const double zero = (double)ldf(t + 168);
  double f0;
  if (x == zero) {
    if (y == zero) {
      if (!x_negative) return;  // f1 stays y
      ctx.f1.f64 = y_negative ? -ldd(t + 16) : ldd(t + 16);
      return;
    }
    f0 = ldd(t + 8);
  } else {
    const double ax = __builtin_fabs(x), ay = __builtin_fabs(y);
    i32 k = 0;
    double den = ax, num = ay;
    if (ay > ax) {
      den = ay;
      k = 2;
      num = ax;
    }
    double r = num / den;
    if (r > ldd(t + 24)) {
      const double s3 = ldd(t + 40);
      ++k;
      const double one = (double)ldf(t + 176);
      const double sum = s3 + r;
      r = (s3 * r + -one) / sum;
    }
    const double z = r * r;
    double q = ldd(t + 112) + z;
    double p = ldd(t + 80) * z + ldd(t + 72);
    q = q * z + ldd(t + 104);
    p = p * z + ldd(t + 64);
    q = q * z + ldd(t + 96);
    p = p * z + ldd(t + 56);
    q = q * z + ldd(t + 88);
    f0 = p * z;
    f0 = f0 * r;
    f0 = f0 / q;
    f0 = f0 + r;
    if (k > 1) f0 = -f0;
    f0 = ldd(t + 128 + (u32)k * 8) + f0;
  }
  const double mirrored = ldd(t + 16) - f0;
  const double result = x >= 0.0 ? f0 : mirrored;
  ctx.f1.f64 = y_negative ? -result : result;
}

// ---------------------------------------------------------------------------
// The scene's draw (guest 0x82065E38): render state (0x8209D388 with the
// device 0x8219D290, 0, 0, 0, 0, 1), the camera (0x82067A20, f1 1), then the
// dynamic vertex streams opened (each: the next buffer of the ring at
// 0x821B4120, its index 0x821B4118 wrapping at 256, as 0x8206A570, here
// inline; recorded in the stream's list (+2048 its length, 8 bytes an
// entry), locked (0x820A1800, D3D), the lock's data at +2052, +2056 the
// vertices used) and closed again (the used count into the list, unlocked,
// 0x820A1850) in the original's order, and between them every entity of
// the draw lists (0x82181350.. +4 heads) not flagged dead drawn by the
// native draw dispatch (0x82045740), with the game's own extra passes
// (0x82047AA8, 0x82030AC0, 0x820614F8 when the byte 0x821B3980 is set,
// 0x8204F4F8, 0x8204FBA8, 0x820749F8) called where the original calls them.
extern "C" void sub_8209D388(PPCContext& ctx, uint8_t* base);  // render state (guest)
extern "C" void sub_82067A20(PPCContext& ctx, uint8_t* base);  // the camera (below)
extern "C" void sub_820A1800(PPCContext& ctx, uint8_t* base);  // vertex buffer lock (D3D)
extern "C" void sub_820A1850(PPCContext& ctx, uint8_t* base);  // vertex buffer unlock (D3D)
extern "C" void sub_82047AA8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030AC0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820614F8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204F4F8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204FBA8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820749F8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82045740(PPCContext& ctx, uint8_t* base);
namespace {

// A dynamic stream opened (lr_next, lr_lock: the original's returns).
void stream_open(PPCContext& ctx, uint8_t* base, u32 s, u32 lr_next, u32 lr_lock) {
  const u32 n = gw_load32(s + 2048);
  const u32 entry = (n << 3) + s;
  gw_store32(s + 2048, n + 1);
  // 0x8206A570: the ring's next buffer.
  ctx.lr = lr_next;
  const u32 index = gw_load32(0x821B0000 + 16664);
  ctx.r3.u64 = gw_load32((index << 2) + 0x821B0000 + 16672);
  gw_store32(0x821B0000 + 16664, (index + 1) & 0xFF);
  ctx.r8.u64 = index << 2;  // its scratch registers, as it leaves them
  ctx.r9.u64 = 0x821B0000 + 16672;
  ctx.r10.u64 = 0x821B0000;
  gw_store32(entry + 4, ctx.r3.u32);
  gw_store32(entry + 0, 0);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.lr = lr_lock;
  sub_820A1800(ctx, base);
  gw_store32(s + 2052, ctx.r3.u32);
  gw_store32(s + 2056, 0);
}

// A dynamic stream closed: its used count into its last list entry, unlocked.
void stream_close(PPCContext& ctx, uint8_t* base, u32 s, u32 lr_unlock) {
  const u32 last = (gw_load32(s + 2048) << 3) + s;
  const u32 used = gw_load32(s + 2056);
  ctx.r3.u64 = gw_load32(last - 4);
  gw_store32(last - 8, used);
  ctx.lr = lr_unlock;
  sub_820A1850(ctx, base);
}

// Every entity of a list whose flags (+36) & mask are clear, drawn.
void draw_list(PPCContext& ctx, uint8_t* base, u32 head, u32 mask, u32 lr) {
  u32 node = gw_load32(head + 4);
  for (u32 e = gw_load32(node); e; e = gw_load32(node)) {
    if (!(gw_load32(e + 36) & mask)) {
      ctx.r3.u64 = e;
      ctx.lr = lr;
      sub_82045740(ctx, base);
    }
    node = gw_load32(node + 4);
    ctx.r3.u64 = gw_load32(node);
  }
  ctx.r3.u64 = 0;
}

}  // namespace

extern "C" void sub_82065E38(PPCContext& ctx, uint8_t* base) {
  const u32 sp = ctx.r1.u32;
  gw_store32(sp - 144, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = sp - 144;
  constexpr u32 A = 0x821A0000, B = 0x821B0000, L = 0x82180000;
  ctx.r8.u64 = 1;
  ctx.r7.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r4.u64 = 0;
  ctx.r3.u64 = gw_load32(A - 11632);
  ctx.lr = 0x82065E64;
  sub_8209D388(ctx, base);
  ctx.f1.f64 = (double)ldf(kOne);
  ctx.lr = 0x82065E70;
  sub_82067A20(ctx, base);
  const u32 sprites = A + 27016, quads = A + 12424, outlines = A + 14536;
  stream_open(ctx, base, sprites, 0x82065E90, 0x82065EAC);
  stream_open(ctx, base, quads, 0x82065ED8, 0x82065EF0);
  stream_open(ctx, base, outlines, 0x82065F1C, 0x82065F34);
  draw_list(ctx, base, L + 5068, 1, 0x82065F6C);
  draw_list(ctx, base, L + 5020, 3, 0x82065FA8);
  ctx.lr = 0x82065FBC;
  sub_82047AA8(ctx, base);
  draw_list(ctx, base, L + 5080, 1, 0x82065FE8);
  draw_list(ctx, base, L + 5044, 1, 0x82066024);
  draw_list(ctx, base, L + 5056, 1, 0x82066060);
  draw_list(ctx, base, L + 5032, 1, 0x8206609C);
  ctx.lr = 0x820660B0;
  sub_82030AC0(ctx, base);
  if (gw_load8(B + 14688 + 32)) {
    ctx.lr = 0x820660C8;
    sub_820614F8(ctx, base);
  }
  ctx.lr = 0x820660CC;
  sub_8204F4F8(ctx, base);
  ctx.lr = 0x820660D0;
  sub_8204FBA8(ctx, base);
  stream_close(ctx, base, outlines, 0x820660EC);
  stream_close(ctx, base, quads, 0x82066108);
  const u32 s2a = B - 28200, s2b = B - 26136;
  stream_open(ctx, base, s2a, 0x82066128, 0x82066140);
  stream_open(ctx, base, s2b, 0x8206616C, 0x82066184);
  draw_list(ctx, base, L + 5200, 1, 0x820661BC);
  stream_close(ctx, base, s2b, 0x820661E8);
  stream_close(ctx, base, s2a, 0x82066204);
  stream_close(ctx, base, sprites, 0x82066220);
  const u32 s3a = A + 29080, s3b = A + 31144, s3c = B - 32328, s3d = B - 30264;
  stream_open(ctx, base, s3a, 0x82066240, 0x82066258);
  stream_open(ctx, base, s3b, 0x82066284, 0x8206629C);
  stream_open(ctx, base, s3c, 0x820662C8, 0x820662E0);
  stream_open(ctx, base, s3d, 0x8206630C, 0x82066324);
  ctx.lr = 0x82066330;
  sub_820749F8(ctx, base);
  stream_close(ctx, base, s3d, 0x8206634C);
  stream_close(ctx, base, s3c, 0x82066368);
  stream_close(ctx, base, s3b, 0x82066384);
  stream_close(ctx, base, s3a, 0x820663A0);
  ctx.r1.u64 = sp;  // (lr left at the last call's return, as the original)
}

// ---------------------------------------------------------------------------
// A glowing 3D line in two colours (guest 0x8205F6B8): r3 and r4 its ends,
// r5 and r6 their colours (r, g, b, a: times a, times 18), f1 its width.
// Four vertices from the stream at 0x821A81B8 (the glow) and eight from
// 0x821A89C8 (the outline), through the game's allocator (0x82061750,
// guest), written big-endian into the game's vertex chunk as the original.
// As 0x82060CC0, the arithmetic follows the original instruction by
// instruction (fN_k: the k-th value of its register fN), the calls with the
// registers it passes; the checker confirms it bit for bit.
extern "C" void sub_8205F6B8(PPCContext& ctx, uint8_t* base) {
  const u32 sp = ctx.r1.u32;
  gw_store32(sp - 224, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = sp - 224;
  u32 r11, r27, r28, r29, r30, r31;
  r11 = (u32)-2112159744;
  r28 = ctx.r3.u32;
  r27 = ctx.r4.u32;
  ctx.r3.u64 = (u32)(r11 + -32328);
  ctx.r4.u64 = (u32)4;
  r30 = ctx.r5.u32;
  r29 = ctx.r6.u32;
  ctx.lr = 0x8205F6EC;
  sub_82061750(ctx, base);
  const double f1_1 = ctx.f1.f64;
  r11 = (u32)-2112159744;
  r31 = ctx.r3.u32;
  r11 = (u32)(r11 + -30264);
  ctx.r4.u64 = (u32)8;
  ctx.r3.u64 = r11;
  ctx.lr = 0x8205F704;
  ctx.f1.f64 = f1_1;
  sub_82061750(ctx, base);
  const double f1_2 = ctx.f1.f64;
  r11 = (u32)-2112225280;
  const float f12_1 = ldf(r30 + 12);
  r11 = (u32)(r11 + 7572);
  const float f2_1 = ldf(r30 + 0);
  const float f1_3 = ldf(r30 + 4);
  const float f2_2 = f12_1 * f2_1;
  const float f31_1 = ldf(r30 + 8);
  const float f1_4 = f1_3 * f12_1;
  const float f3_1 = ldf(r29 + 12);
  const float f31_2 = f31_1 * f12_1;
  const float f30_1 = ldf(r29 + 0);
  const float f0_1 = ldf(r11 + 32);
  r11 = (u32)-2113929216;
  const float f29_1 = ldf(r29 + 4);
  const float f30_2 = f3_1 * f30_1;
  const float f27_1 = ldf(r29 + 8);
  const float f29_2 = f29_1 * f3_1;
  const float f10_1 = ldf(r28 + 0);
  const float f27_2 = f27_1 * f3_1;
  const float f8_1 = ldf(r27 + 0);
  const float f4_1 = ldf(r11 + 3208);
  r11 = (u32)-2113929216;
  const float f9_1 = ldf(r28 + 4);
  const float f11_1 = f8_1 - f10_1;
  const float f7_1 = ldf(r27 + 4);
  const float f28_1 = f7_1 - f9_1;
  const float f6_1 = ldf(r28 + 8);
  const float f5_1 = ldf(r27 + 8);
  const float f13_1 = ldf(r11 + 1732);
  r11 = (u32)-2113929216;
  const float f26_1 = f5_1 - f6_1;
  const float f12_2 = ldf(r11 + 10024);
  r11 = (u32)-2113929216;
  const float f3_2 = f2_2 * f12_2;
  const float f2_3 = f1_4 * f12_2;
  const float f1_5 = f31_2 * f12_2;
  const float f31_3 = f30_2 * f12_2;
  const float f30_3 = f29_2 * f12_2;
  const float f29_3 = f27_2 * f12_2;
  const double f12_3 = (double)f11_1 * (double)f11_1;
  const double f27_3 = (double)f28_1 * (double)f28_1;
  const double f25_1 = ((double)f26_1 * (double)f26_1 + f12_3);
  const double f24_1 = f12_3 + f27_3;
  const double f12_4 = ldd(r11 + 10080);
  const double f27_4 = f25_1 + f27_3;
  const double f25_2 = f24_1 + f12_4;
  const double f27_5 = f27_4 + f12_4;
  const float f12_5 = (float)(1.0 / __builtin_sqrt(f25_2));
  const float f25_3 = (float)(1.0 / __builtin_sqrt(f27_5));
  const double f24_2 = (double)f12_5 * (double)f11_1;
  const double f12_6 = (double)f12_5 * (double)f28_1;
  const double f27_6 = (double)f25_3 * (double)f11_1;
  const double f11_2 = -f24_2;
  const double f28_2 = (double)f25_3 * (double)f28_1;
  const double f26_2 = (double)f25_3 * (double)f26_1;
  const double f25_4 = -((double)f0_1 * f12_6 + -(double)f10_1);
  const double f24_3 = -((double)f0_1 * f11_2 + -(double)f9_1);
  const float f25_5 = (float)f25_4;
  stf(r31 + 0, f25_5);
  const float f25_6 = (float)f24_3;
  stf(r31 + 4, f25_6);
  stf(r31 + 8, f6_1);
  stf(r31 + 12, f4_1);
  stf(r31 + 16, f3_2);
  stf(r31 + 20, f2_3);
  stf(r31 + 24, f1_5);
  stf(r31 + 28, f13_1);
  const double f25_7 = ((double)f0_1 * f12_6 + (double)f10_1);
  r11 = (u32)(r31 + 32);
  const double f24_4 = ((double)f0_1 * f11_2 + (double)f9_1);
  ctx.r10.u64 = (u32)-2113929216;
  const double f23_1 = ((double)f0_1 * f12_6 + (double)f8_1);
  const double f22_1 = ((double)f0_1 * f11_2 + (double)f7_1);
  const double f21_1 = -((double)f0_1 * f12_6 + -(double)f8_1);
  const double f20_1 = -((double)f0_1 * f11_2 + -(double)f7_1);
  const float f25_8 = (float)f25_7;
  stf(r11 + 0, f25_8);
  const float f24_5 = (float)f24_4;
  stf(r11 + 4, f24_5);
  stf(r11 + 8, f6_1);
  const float f23_2 = (float)f23_1;
  stf(r11 + 12, f13_1);
  const float f22_2 = (float)f22_1;
  stf(r11 + 16, f3_2);
  const float f25_9 = (float)f21_1;
  stf(r11 + 20, f2_3);
  const double f21_2 = -((double)f0_1 * f12_6 + -(double)f10_1);
  stf(r11 + 24, f1_5);
  const float f24_6 = (float)f20_1;
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  const double f20_2 = -((double)f0_1 * f26_2 + -(double)f6_1);
  stf(r11 + 0, f23_2);
  stf(r11 + 4, f22_2);
  const double f22_3 = ((double)f0_1 * f11_2 + (double)f9_1);
  stf(r11 + 8, f5_1);
  stf(r11 + 12, f13_1);
  const float f21_3 = (float)f21_2;
  stf(r11 + 16, f31_3);
  stf(r11 + 20, f30_3);
  stf(r11 + 24, f29_3);
  const float f20_3 = (float)f20_2;
  stf(r11 + 28, f13_1);
  stf(r11 + 32, f25_9);
  const double f25_10 = -((double)f0_1 * f11_2 + -(double)f9_1);
  stf(r11 + 36, f24_6);
  const double f24_7 = ((double)f0_1 * f12_6 + (double)f10_1);
  stf(r11 + 40, f5_1);
  stf(r11 + 44, f4_1);
  const double f22_4 = -((double)f0_1 * f28_2 + -f22_3);
  stf(r11 + 48, f31_3);
  stf(r11 + 52, f30_3);
  stf(r11 + 56, f29_3);
  stf(r11 + 60, f13_1);
  r11 = (u32)(ctx.r3.u32 + 32);
  stf(ctx.r3.u32 + 0, f21_3);
  const float f25_11 = (float)f25_10;
  stf(ctx.r3.u32 + 4, f25_11);
  const double f25_12 = ((double)f0_1 * f11_2 + (double)f9_1);
  stf(ctx.r3.u32 + 8, f6_1);
  stf(ctx.r3.u32 + 12, f4_1);
  const float f24_8 = (float)f24_7;
  stf(ctx.r3.u32 + 16, f3_2);
  const float f22_5 = (float)f22_4;
  stf(ctx.r3.u32 + 20, f2_3);
  stf(ctx.r3.u32 + 24, f1_5);
  stf(ctx.r3.u32 + 28, f13_1);
  stf(r11 + 0, f24_8);
  const float f23_3 = (float)f25_12;
  stf(r11 + 4, f23_3);
  const double f25_13 = ((double)f0_1 * f12_6 + (double)f10_1);
  stf(r11 + 8, f6_1);
  stf(r11 + 12, f13_1);
  stf(r11 + 16, f3_2);
  stf(r11 + 20, f2_3);
  stf(r11 + 24, f1_5);
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  const double f21_4 = -((double)f0_1 * f27_6 + -f25_13);
  const float f25_14 = ldf(ctx.r10.u32 + 1740);
  const float f21_5 = (float)f21_4;
  stf(r11 + 0, f21_5);
  stf(r11 + 4, f22_5);
  stf(r11 + 8, f20_3);
  stf(r11 + 12, f13_1);
  stf(r11 + 16, f3_2);
  stf(r11 + 20, f2_3);
  stf(r11 + 24, f1_5);
  const double f10_2 = -((double)f0_1 * f12_6 + -(double)f10_1);
  stf(r11 + 28, f25_14);
  const double f9_2 = -((double)f0_1 * f11_2 + -(double)f9_1);
  r11 = (u32)(r11 + 32);
  const double f6_2 = -((double)f0_1 * f26_2 + -(double)f6_1);
  const double f10_3 = -((double)f0_1 * f27_6 + -f10_2);
  const double f9_3 = -((double)f0_1 * f28_2 + -f9_2);
  const float f10_4 = (float)f10_3;
  stf(r11 + 0, f10_4);
  const float f9_4 = (float)f9_3;
  stf(r11 + 4, f9_4);
  const float f10_5 = (float)f6_2;
  stf(r11 + 8, f10_5);
  const double f10_6 = -((double)f0_1 * f12_6 + -(double)f8_1);
  stf(r11 + 12, f4_1);
  stf(r11 + 16, f3_2);
  const double f9_5 = -((double)f0_1 * f11_2 + -(double)f7_1);
  stf(r11 + 20, f2_3);
  const double f6_3 = ((double)f0_1 * f12_6 + (double)f8_1);
  stf(r11 + 24, f1_5);
  const double f3_3 = ((double)f0_1 * f11_2 + (double)f7_1);
  stf(r11 + 28, f25_14);
  r11 = (u32)(r11 + 32);
  const float f10_7 = (float)f10_6;
  stf(r11 + 0, f10_7);
  const float f9_6 = (float)f9_5;
  stf(r11 + 4, f9_6);
  stf(r11 + 8, f5_1);
  const double f10_8 = f12_6 + f27_6;
  stf(r11 + 12, f4_1);
  const double f9_7 = f11_2 + f28_2;
  stf(r11 + 16, f31_3);
  const double f12_7 = -((double)f0_1 * f12_6 + -(double)f8_1);
  stf(r11 + 20, f30_3);
  const float f6_4 = (float)f6_3;
  stf(r11 + 24, f29_3);
  const double f11_3 = -((double)f0_1 * f11_2 + -(double)f7_1);
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  const float f3_4 = (float)f3_3;
  stf(r11 + 0, f6_4);
  const double f10_9 = (f10_8 * (double)f0_1 + (double)f8_1);
  stf(r11 + 4, f3_4);
  const double f9_8 = (f9_7 * (double)f0_1 + (double)f7_1);
  stf(r11 + 8, f5_1);
  const double f6_5 = ((double)f0_1 * f26_2 + (double)f5_1);
  stf(r11 + 12, f13_1);
  const double f12_8 = ((double)f0_1 * f27_6 + f12_7);
  stf(r11 + 16, f31_3);
  const double f11_4 = ((double)f0_1 * f28_2 + f11_3);
  stf(r11 + 20, f30_3);
  const double f0_2 = ((double)f0_1 * f26_2 + (double)f5_1);
  stf(r11 + 24, f29_3);
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  const float f10_10 = (float)f10_9;
  const float f9_9 = (float)f9_8;
  stf(r11 + 0, f10_10);
  const float f6_6 = (float)f6_5;
  stf(r11 + 4, f9_9);
  const float f12_9 = (float)f12_8;
  stf(r11 + 8, f6_6);
  stf(r11 + 12, f13_1);
  const float f13_2 = (float)f11_4;
  stf(r11 + 16, f31_3);
  const float f0_3 = (float)f0_2;
  stf(r11 + 20, f30_3);
  stf(r11 + 24, f29_3);
  stf(r11 + 28, f25_14);
  stf(r11 + 32, f12_9);
  stf(r11 + 36, f13_2);
  stf(r11 + 40, f0_3);
  stf(r11 + 44, f4_1);
  stf(r11 + 48, f31_3);
  stf(r11 + 52, f30_3);
  stf(r11 + 56, f29_3);
  stf(r11 + 60, f25_14);
  ctx.r1.u64 = sp;
  ctx.lr = 0x8205FA94;  // as the original leaves it (its register-restore call)
}

// ---------------------------------------------------------------------------
// A blinking entity's update (guest 0x8204D8E0, r3 the entity): its age
// +104 runs with the time step; past its life +116 without an owner (+72)
// it ends (0x82035A40, guest); in its last second it blinks: with an owner
// its colour +40..+52 back to +120..+132 (the byte +112 cleared), else the
// CRT routine 0x8212FC00 on (15 x age, 1.0) switches the colour off (above
// 0.5) and back on (below 0.5), the byte +112 holding which. Its velocity
// is damped by 0.985 when its speed^2 is over 1024; its angle +20 runs at
// 5 a second; then 0x8204D2E8 (guest) with f1 1.
extern "C" void sub_82035A40(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204D2E8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204D8E0(PPCContext& ctx, uint8_t* base) {
  const u32 self = ctx.r3.u32;
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32;
  gw_store32(sp - 112, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = sp - 112;
  const float age = ldf(kDeltaTime) + ldf(self + 104);
  const float life = ldf(self + 116);
  const float one = ldf(kOne);
  stf(self + 104, age);
  auto restore = [&]() {
    for (u32 k = 0; k < 16; k += 4) gw_store32(self + 40 + k, gw_load32(self + 120 + k));
    gw_store8(self + 112, 0);
  };
  if (!(age < life) && gw_load32(self + 72) == 0) {
    ctx.r3.u64 = self;
    ctx.lr = 0x8204D934;
    sub_82035A40(ctx, base);
  } else if (!(age < life - one)) {
    if (gw_load32(self + 72)) {
      restore();
    } else {
      const bool off = gw_load8(self + 112) != 0;
      ctx.f1.f64 = (double)(age * ldf(C + 3364));
      ctx.f2.f64 = ldd(C + 3192);
      ctx.r3.u64 = self;
      ctx.lr = off ? 0x8204D9A0 : 0x8204D9E4;
      sub_8212FC00(ctx, base);
      const float f = (float)ctx.f1.f64;
      const float half = ldf(kHalf);
      if (off) {
        if (f < half) restore();
      } else if (f > half) {
        const float zero = ldf(kZero);
        for (u32 k = 0; k < 16; k += 4) stf(self + 40 + k, zero);
        gw_store8(self + 112, 1);
      }
    }
  }
  {
    const float vx = ldf(self + 12), vy = ldf(self + 16);
    if (vx * vx + vy * vy > ldf(C + 7280)) {
      const float damp = ldf(C + 7096);
      const float wy = ldf(self + 16);
      stf(self + 12, ldf(self + 12) * damp);
      stf(self + 16, wy * damp);
    }
  }
  stf(self + 20, (float)((double)ldf(kDeltaTime) * (double)ldf(C + 1776) + (double)ldf(self + 20)));
  ctx.r3.u64 = self;
  ctx.f1.f64 = (double)one;
  ctx.lr = 0x8204DA80;
  sub_8204D2E8(ctx, base);
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// Model drawing: three entity draws (guest 0x82056FA0, 0x82046D88,
// 0x82047DF8, r3 the entity) that differ only in their colour. Through the
// entity's handle (+120/+124/+126; nothing when dead) its owner, and the
// owner's handle (+4/+8/+10) the model; the entity's matrix from
// 0x82045CF0 (below, into the frame). Fading (flag 0x10): two passes, each
// pulsing as 0x82045628's (the CRT routine 0x8212FC00, folded past 1, drawn
// while the fold is at least 0x82000D64), the matrix scaled by
// 0.1 + 0x82000FF8 x the pulse (the identity times it, 0x82040F28, then
// times the matrix, 0x82041CA0; both here in registers, the product written
// where the original's lands) and drawn (0x82061DA0, src/shapes_native.cpp)
// with the colour's alpha times the fold; else drawn once with the colour.
// Colours: 0x82056FA0 the owner's +40..+52; 0x82046D88 white;
// 0x82047DF8 white brightened by 2 x +148 (alpha 1).
extern "C" void sub_82045CF0(PPCContext& ctx, uint8_t* base);  // the entity's matrix (below)
extern "C" void sub_82061DA0(PPCContext& ctx, uint8_t* base);  // a model's lines (src/shapes_native.cpp)
namespace {

struct ModelDraw {
  u32 frame_size, matrix, slot, identity, scaled, product, colour_off;
  u32 lr_matrix, lr_pow, lr_fade_draw, lr_draw, lr_end, lr_fade_end;
  int kind;  // 0: the owner's colour, 1: white, 2: brightened white
};

void model_draw(PPCContext& ctx, uint8_t* base, const ModelDraw& d) {
  const u32 self = ctx.r3.u32;
  const u32 sp = ctx.r1.u32, frame = sp - d.frame_size;
  gw_store32(frame, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = frame;
  auto owner = [&]() -> u32 {
    const u32 h = gw_load32(self + 120);
    if (gw_load16(h) != gw_load16(self + 124)) return 0;
    const u32 p = gw_load32(h + 4);
    if (!p) return 0;
    return p + gw_load16(self + 126);
  };
  // The owner's model (its own handle), or 0; p 0 reads as the original's
  // (from address 4 on).
  auto model = [&](u32 p) -> u32 {
    const u32 h = gw_load32(p + 4);
    if (gw_load16(h) != gw_load16(p + 8)) return 0;
    const u32 q = gw_load32(h + 4);
    if (!q) return 0;
    return gw_load16(p + 10) + q;
  };
  bool faded = false;
  if (owner()) {
    const u32 m = frame + d.matrix;
    ctx.r4.u64 = self;
    ctx.r3.u64 = m;
    ctx.lr = d.lr_matrix;
    sub_82045CF0(ctx, base);
    if (gw_load32(self + 36) & 0x10) {
      faded = true;
      const float zero = ldf(kZero), base_k = ldf(C + 1812), step = ldf(C + 4088), least = ldf(C + 3428);
      const float two = ldf(C + 1788), one = ldf(kOne), three = ldf(C + 1784);
      const double unit = ldd(C + 3192);
      const u32 colour = frame + 96;
      for (i32 pass = 0; pass < 2; ++pass) {
        ctx.f2.f64 = unit;
        ctx.f1.f64 = (double)(ldf(self + 56) * three);
        ctx.lr = d.lr_pow;
        sub_8212FC00(ctx, base);
        gw_store64(frame + d.slot, (u64)(i64)pass);  // (the original's conversion slot)
        const float p = (float)ctx.f1.f64 + (float)(double)(i64)pass;
        const float fold = p < one ? p : two - p;
        if (fold < least) continue;
        if (d.kind == 0) {
          stf(colour + 12, ldf(self + 52) * fold);
          stf(colour + 4, ldf(self + 44));
          stf(colour + 0, ldf(self + 40));
          stf(colour + 8, ldf(self + 48));
        } else {
          stf(colour + 0, one);
          stf(colour + 4, one);
          stf(colour + 8, one);
          stf(colour + 12, fold);
        }
        const u32 shape = model(owner());
        const float k = (float)((double)p * (double)step + (double)base_k);
        // The identity scaled by (k, k, k) (0x82040F28), its last row kept.
        const float id[16] = {one, zero, zero, zero, zero, one, zero, zero,
                              zero, zero, one, zero, zero, zero, zero, one};
        float s[16];
        for (u32 i = 0; i < 12; ++i) s[i] = id[i] * k;
        for (u32 i = 12; i < 16; ++i) s[i] = id[i];
        // Times the entity's matrix (0x82041CA0's arithmetic).
        float mm[16];
        for (u32 i = 0; i < 16; ++i) mm[i] = ldf(m + i * 4);
        const u32 out = frame + d.product;
        for (u32 row = 0; row < 4; ++row) {
          const float v0 = s[row * 4], v1 = s[row * 4 + 1], v2 = s[row * 4 + 2], v3 = s[row * 4 + 3];
          for (u32 c = 0; c < 4; ++c) {
            float t = v0 * mm[c] + v1 * mm[4 + c];
            t = t + v2 * mm[8 + c];
            stf(out + row * 16 + c * 4, t + v3 * mm[12 + c]);
          }
        }
        ctx.r3.u64 = out;
        ctx.r4.u64 = shape;
        ctx.r5.u64 = colour;
        ctx.r6.u64 = 0;
        ctx.lr = d.lr_fade_draw;
        sub_82061DA0(ctx, base);
      }
    } else {
      u32 colour = self + 40;
      if (d.kind != 0) {
        const float one = ldf(kOne);
        colour = frame + d.colour_off;
        const float c = d.kind == 1 ? one : (float)((double)ldf(self + 148) * (double)ldf(C + 1788) + (double)one);
        stf(colour + 0, c);
        stf(colour + 4, c);
        stf(colour + 8, c);
        stf(colour + 12, one);
      }
      ctx.r4.u64 = model(owner());
      ctx.r6.u64 = 0;
      ctx.r5.u64 = colour;
      ctx.r3.u64 = m;
      ctx.lr = d.lr_draw;
      sub_82061DA0(ctx, base);
    }
  }
  ctx.r1.u64 = sp;
  // As the original leaves it: its register-restore call, a different one
  // after the fading passes.
  ctx.lr = faded ? d.lr_fade_end : d.lr_end;
}

}  // namespace

extern "C" void sub_82056FA0(PPCContext& ctx, uint8_t* base) {
  static const ModelDraw d = {496, 192, 176, 112, 256, 320, 0,
                              0x82056FF4, 0x82057058, 0x820571A0, 0x8205722C, 0x82057238, 0x820571B8, 0};
  model_draw(ctx, base, d);
}
extern "C" void sub_82046D88(PPCContext& ctx, uint8_t* base) {
  static const ModelDraw d = {512, 208, 192, 128, 272, 336, 80,
                              0x82046DDC, 0x82046E40, 0x82046F74, 0x82047018, 0x82047024, 0x82046F8C, 1};
  model_draw(ctx, base, d);
}
extern "C" void sub_82047DF8(PPCContext& ctx, uint8_t* base) {
  static const ModelDraw d = {512, 208, 192, 128, 272, 336, 80,
                              0x82047E4C, 0x82047EB0, 0x82047FE4, 0x82048098, 0x820480A4, 0x82047FFC, 2};
  model_draw(ctx, base, d);
}

// ---------------------------------------------------------------------------
// An entity's world matrix (guest 0x82045CF0): r3 the 4 x 4 out, r4 the
// entity; by its owner's mode (+12 through the handle at +120): 3 the
// entity's virtual +24; 2 facing the player it goes for (0x82045AB8): a
// look-at (0x82047180) from its position along the way to the player
// (scaled by max(d, 100) / max(d, 0.1), lifted by 100) with the owner's
// up (+16..+24); 1 a look-at along its velocity (z 100) with the owner's
// +16..+24 as forward-up; then (modes 1, 2) times a rotation about z by
// its angle +20; any other mode: that rotation with the position added to
// its last row (0x82040FF0, here inline). The vectors pass through the
// original's frame as it lays them out (the natives 0x82041600, 0x82047180
// and 0x82041CA0 read them there). Returns r3.
extern "C" void sub_82041CA0(PPCContext& ctx, uint8_t* base);  // matrix times matrix (src/math_native.cpp)
extern "C" void sub_82045CF0(PPCContext& ctx, uint8_t* base) {
  const u32 out = ctx.r3.u32, self = ctx.r4.u32;
  const u32 sp = ctx.r1.u32, f = sp - 496;
  ctx.lr = 0x82045CF8;  // (lr left at the last call's return, as the original)
  gw_store32(f, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = f;
  auto owner = [&]() -> u32 {
    const u32 h = gw_load32(self + 120);
    if (gw_load16(h) != gw_load16(self + 124)) return 0;
    const u32 p = gw_load32(h + 4);
    if (!p) return 0;
    return p + gw_load16(self + 126);
  };
  const i32 mode = (i32)gw_load32(owner() + 12);
  const float zero = ldf(kZero), one = ldf(kOne);
  if (mode == 3) {
    ctx.r4.u64 = self;
    ctx.r3.u64 = out;
    const u32 fn = gw_load32(gw_load32(self) + 24);
    ctx.lr = 0x82045DD0;
    REX_CALL_INDIRECT_FUNC(fn);
    ctx.r3.u64 = out;
    ctx.r1.u64 = sp;
    return;
  }
  if (mode != 1 && mode != 2) {
    stf(f + 80, ldf(self + 4));
    stf(f + 84, ldf(self + 8));
    stf(f + 88, zero);
    stf(f + 92, zero);
    stf(f + 96, zero);
    stf(f + 100, zero);
    stf(f + 104, one);
    stf(f + 108, zero);
    ctx.r3.u64 = f + 144;
    ctx.r4.u64 = f + 96;
    ctx.f1.f64 = (double)ldf(self + 20);
    ctx.lr = 0x82045D9C;
    sub_82041600(ctx, base);
    // 0x82040FF0: the rotation with the position added to its last row.
    const u32 m = ctx.r3.u32, v = f + 80;
    const float t0 = ldf(m + 48) + ldf(v + 0), t1 = ldf(m + 52) + ldf(v + 4);
    const float t2 = ldf(m + 56) + ldf(v + 8), t3 = ldf(m + 60) + ldf(v + 12);
    for (u32 k = 0; k < 48; k += 4) gw_store32(out + k, gw_load32(m + k));
    stf(out + 48, t0);
    stf(out + 52, t1);
    stf(out + 56, t2);
    stf(out + 60, t3);
    ctx.lr = 0x82045DAC;
    ctx.r3.u64 = out;
    ctx.r1.u64 = sp;
    return;
  }
  u32 look, axis, rot;
  const float far = ldf(C + 4128);
  if (mode == 2) {
    ctx.r3.u64 = self;
    ctx.lr = 0x82045DE4;
    sub_82045AB8(ctx, base);
    const u32 player = ctx.r3.u32;
    if (player) {
      const float dx = ldf(player + 4) - ldf(self + 4);
      const float dy = ldf(player + 8) - ldf(self + 8);
      const float d = sqrt_ss(dy * dy + dx * dx);
      const float tenth = ldf(C + 1812);
      const float a = d - far >= 0 ? d : far;
      const float b = d - tenth >= 0 ? d : tenth;
      const float k = a / b;
      const float z = k * zero;
      stf(f + 92, z);
      stf(f + 80, k * dx);
      stf(f + 84, k * dy);
      stf(f + 88, z + far);
    } else {
      stf(f + 88, one);
      stf(f + 80, zero);
      stf(f + 84, zero);
      stf(f + 92, zero);
    }
    const u32 p = owner();
    stf(f + 96, ldf(self + 4));
    stf(f + 100, ldf(self + 8));
    stf(f + 112, ldf(p + 16));
    stf(f + 116, ldf(p + 20));
    stf(f + 124, zero);
    stf(f + 104, zero);
    stf(f + 108, one);
    stf(f + 128, zero);
    stf(f + 132, zero);
    stf(f + 136, one);
    stf(f + 140, zero);
    stf(f + 120, ldf(p + 24));
    ctx.r3.u64 = f + 336;
    ctx.r4.u64 = f + 96;
    ctx.r5.u64 = f + 80;
    ctx.r6.u64 = f + 112;
    ctx.lr = 0x82045F14;
    look = f + 336;
    axis = f + 128;
    rot = f + 208;
  } else {
    const u32 p = owner();
    stf(f + 128, ldf(self + 12));
    stf(f + 132, ldf(self + 16));
    stf(f + 112, ldf(self + 4));
    stf(f + 116, ldf(self + 8));
    stf(f + 108, zero);
    stf(f + 140, zero);
    stf(f + 120, zero);
    stf(f + 80, zero);
    stf(f + 84, zero);
    stf(f + 92, zero);
    stf(f + 96, ldf(p + 16));
    stf(f + 136, far);
    stf(f + 100, ldf(p + 20));
    stf(f + 124, one);
    stf(f + 88, one);
    stf(f + 104, ldf(p + 24));
    ctx.r3.u64 = f + 272;
    ctx.r4.u64 = f + 112;
    ctx.r5.u64 = f + 128;
    ctx.r6.u64 = f + 96;
    ctx.lr = 0x82045FDC;
    look = f + 272;
    axis = f + 80;
    rot = f + 400;
  }
  sub_82047180(ctx, base);
  ctx.r3.u64 = rot;
  ctx.r4.u64 = axis;
  ctx.f1.f64 = (double)ldf(self + 20);
  ctx.lr = 0x82045FF0;
  sub_82041600(ctx, base);
  ctx.r4.u64 = ctx.r3.u32;
  ctx.r5.u64 = look;
  ctx.r3.u64 = out;
  ctx.lr = 0x82046000;
  sub_82041CA0(ctx, base);
  ctx.r3.u64 = out;
  ctx.r1.u64 = sp;
}

// ---------------------------------------------------------------------------
// An explosion's burst (guest 0x82030C58): r3 the particle set, f1 its
// size, r5 how many. Unless the particle store says no (0x82062FC8, guest:
// the coordinator's particle store), two colours a random step apart on the
// shifting colour wheel (0x82030678, from r3's colour), then (count x
// 0x821992A8) / 12 rounds of: a random direction (0x82030168, guest) of
// the size times 0x821992DC, and twelve particles (0x82067388, guest) along
// its six axis permutations and their negatives, each half with its own
// random blend of the two colours. The game's generator (0x82030058) as
// the original calls it; the vectors through the frame as it lays them out.
extern "C" void sub_82062FC8(PPCContext& ctx, uint8_t* base);  // the particle store's check (guest)
extern "C" void sub_82030168(PPCContext& ctx, uint8_t* base);  // a random vector (guest)
extern "C" void sub_82067388(PPCContext& ctx, uint8_t* base);  // add a particle (guest)
extern "C" void sub_82030C58(PPCContext& ctx, uint8_t* base) {
  const u32 set = ctx.r3.u32;
  const double size = ctx.f1.f64;
  const u32 count = ctx.r5.u32;
  const u32 sp = ctx.r1.u32, f = sp - 464;
  ctx.lr = 0x82030C68;  // (lr left at the last call's return, as the original)
  gw_store32(f, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = f;
  ctx.lr = 0x82030C7C;
  sub_82062FC8(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    ctx.r1.u64 = sp;
    ctx.lr = 0x820310C4;
    return;
  }
  constexpr u32 kGen = 0x821A1E28;
  ctx.r3.u64 = kGen;
  ctx.lr = 0x82030C98;
  sub_82030058(ctx, base);
  ctx.r4.u64 = set;
  {
    const u64 r = ctx.r3.u64 & 0xFFFFFFFF;
    gw_store64(f + 96, r);  // (the original's conversion slot)
    ctx.r3.u64 = f + 80;
    const float x = (float)(double)(i64)r;
    const float w = (float)((double)x * (double)ldf(C + 1736) - (double)ldf(kOne));
    const float step = ldf(C + 1812);
    const float c = w * ldf(C + 1816);
    ctx.f1.f64 = (double)(c - step);
    ctx.lr = 0x82030CE4;
    sub_82030678(ctx, base);
    ctx.r4.u64 = set;
    ctx.f1.f64 = (double)(c + step);
    ctx.r3.u64 = f + 320;
    ctx.lr = 0x82030CF4;
    sub_82030678(ctx, base);
  }
  const u32 c2 = ctx.r3.u32;
  const float k = ldf(C + 1808);
  const float b0 = ldf(c2 + 0) * k, b1 = ldf(c2 + 4) * k, b3 = ldf(c2 + 12) * k;
  const float radius = (float)((double)ldf(0x821A0000 - 27972 + 32) * size);
  gw_store64(f + 96, (u64)(i64)(i32)count);
  const float b2 = ldf(c2 + 8) * k;
  const float n = (float)(double)(i64)(i32)count * ldf(0x821A0000 - 28024 + 32);
  const i32 ni = trunc_i32(n);
  gw_store32(f + 96, (u32)ni);
  i32 rounds = ni / 12;
  ctx.r8.u64 = 12;
  if (rounds > 0) {
    const float a0 = ldf(f + 80), a1 = ldf(f + 84), a2 = ldf(f + 88), a3 = ldf(f + 92);
    const float d0 = b0 - a0, d1 = b1 - a1, d2 = b2 - a2, d3 = b3 - a3;
    const float zero = ldf(kZero), unit = ldf(C + 1764);
    const u32 colour = f + 112;
    auto set_colour = [&](u32 slot) {
      const float t = (float)(double)(i64)(ctx.r3.u64 & 0xFFFFFFFF) * unit;
      gw_store64(f + slot, ctx.r3.u64 & 0xFFFFFFFF);
      stf(colour + 0, d0 * t + a0);
      stf(colour + 4, d1 * t + a1);
      stf(colour + 8, d2 * t + a2);
      stf(colour + 12, d3 * t + a3);
    };
    // A particle along (p, q, r) of the random vector, sign s.
    auto emit = [&](u32 at, u32 p, u32 q, u32 r, bool negate, u32 lr) {
      const float vp = ldf(f + 80 + p * 4), vq = ldf(f + 80 + q * 4), vr = ldf(f + 80 + r * 4);
      stf(at + 0, negate ? -vp : vp);
      stf(at + 4, negate ? -vq : vq);
      stf(at + 8, negate ? -vr : vr);
      stf(at + 12, zero);
      ctx.r3.u64 = set;
      ctx.r4.u64 = at;
      ctx.r5.u64 = colour;
      ctx.lr = lr;
      sub_82067388(ctx, base);
    };
    for (; rounds > 0; --rounds) {
      ctx.r3.u64 = f + 80;
      ctx.f1.f64 = (double)radius;
      ctx.r4.u64 = kGen;
      ctx.lr = 0x82030DB4;
      sub_82030168(ctx, base);
      ctx.r3.u64 = kGen;
      ctx.lr = 0x82030DBC;
      sub_82030058(ctx, base);
      set_colour(96);
      emit(f + 288, 0, 1, 2, false, 0x82030E30);
      emit(f + 176, 1, 0, 2, false, 0x82030E5C);
      emit(f + 256, 0, 2, 1, false, 0x82030E88);
      emit(f + 128, 1, 2, 0, false, 0x82030EB4);
      emit(f + 208, 2, 0, 1, false, 0x82030EE0);
      emit(f + 144, 2, 1, 0, false, 0x82030F0C);
      ctx.r3.u64 = kGen;
      ctx.lr = 0x82030F14;
      sub_82030058(ctx, base);
      set_colour(320);
      emit(f + 160, 0, 1, 2, true, 0x82030F94);
      emit(f + 192, 1, 0, 2, true, 0x82030FCC);
      emit(f + 240, 0, 2, 1, true, 0x82031004);
      emit(f + 272, 1, 2, 0, true, 0x8203103C);
      emit(f + 304, 2, 0, 1, true, 0x82031074);
      emit(f + 224, 2, 1, 0, true, 0x820310AC);
    }
  }
  ctx.r1.u64 = sp;
  ctx.lr = 0x820310C4;
}

// ---------------------------------------------------------------------------
// The camera (guest 0x82067A20, f1 unused here as by the original's body):
// a look-at (0x82047180, src/enemy_native.cpp) from the eye (0x821A1D88 +
// 0x821A1D80, height 512) along the camera's offset, up (0, -1, 0); held
// (the byte 0x821B3F84 set: the view at 0x82182618 used instead) or kept
// there; copied to 0x82182658; the projection (0x8206A1C8 and 0x8206A300,
// the game's D3D-side helpers, guest) for a field of view from 0x821B3FCC
// / D+9708, aspect 16:9, far 10000; scaled by 0x821B4000 and shifted by
// 0x821B4034 / 0x821B4068; times the view (0x82041CA0), transposed and
// set as vertex shader constants 0-3 (0x820A6438, D3D), then 0x8209C1B8
// (guest) with 1. The vectors and matrices pass through the original's
// frame as it lays them out.
extern "C" void sub_8206A1C8(PPCContext& ctx, uint8_t* base);  // D3D range: called, not touched
extern "C" void sub_8206A300(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A6438(PPCContext& ctx, uint8_t* base);  // vertex shader constants (D3D)
extern "C" void sub_8209C1B8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82067A20(PPCContext& ctx, uint8_t* base) {
  const u64 lr = ctx.lr;
  const u32 sp = ctx.r1.u32, f = sp - 432;
  gw_store32(f, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = f;
  constexpr u32 A = 0x821A0000, B = 0x821B0000;
  const float zero = ldf(kZero), one = ldf(kOne);
  {
    const float ex = ldf(A + 7560) + ldf(A + 7552);
    stf(f + 176, ex);
    const float ey = ldf(A + 7564) + ldf(A + 7556);
    stf(f + 180, ey);
    stf(f + 184, ldf(C + 11712));
    stf(f + 188, one);
    const float dy = ey - ey;
    stf(f + 144, zero);
    stf(f + 152, zero);
    stf(f + 156, zero);
    stf(f + 148, ldf(C + 3208));
    const float dx = ex - ex;
    const float l = dy * dy + dx * dx;
    const float inv = one / sqrt_ss(l + ldf(C + 11708));
    stf(f + 160, inv * dx);
    stf(f + 164, dy * inv);
    const float z = inv * zero;
    const float w = inv * ldf(C + 11704);
    stf(f + 172, z);
    stf(f + 168, w);
    ctx.r6.u64 = f + 144;
    ctx.r5.u64 = f + 160;
    ctx.r4.u64 = f + 176;
    ctx.r3.u64 = f + 80;
    ctx.lr = 0x82067B08;
    sub_82047180(ctx, base);
  }
  const u32 held = 0x82180000 + 9752, view = f + 80;
  u32 m[16];
  if (gw_load8(B + 16228 + 32)) {
    for (u32 k = 0; k < 16; ++k) m[k] = gw_load32(held + k * 4);
    for (u32 k = 0; k < 16; ++k) gw_store32(view + k * 4, m[k]);
  } else {
    for (u32 k = 0; k < 16; ++k) m[k] = gw_load32(view + k * 4);
    for (u32 k = 0; k < 16; ++k) gw_store32(held + k * 4, m[k]);
  }
  for (u32 k = 0; k < 16; ++k) gw_store32(0x82180000 + 9816 + k * 4, m[k]);
  ctx.r4.u64 = view;
  ctx.r3.u64 = f + 320;
  ctx.lr = 0x82067C7C;
  sub_8206A1C8(ctx, base);
  {
    ctx.f3.f64 = (double)one;
    ctx.r3.u64 = view;
    const float fov = ldf(B + 16300 + 32) / ldf(0x82180000 + 9708);
    ctx.f4.f64 = (double)ldf(C + 11700);
    ctx.f2.f64 = (double)ldf(C + 11696);
    const float half = ldf(C + 3964) / fov;
    ctx.f1.f64 = (double)(half * ldf(C + 8364));
    ctx.lr = 0x82067CC8;
    sub_8206A300(ctx, base);
  }
  {
    const float s = ldf(B + 16352 + 32);
    for (u32 k = 0; k < 32; k += 4) stf(view + k, ldf(view + k) * s);
    stf(view + 32, ldf(view + 32) - ldf(B + 16404 + 32));
    stf(view + 36, ldf(B + 16456 + 32) + ldf(view + 36));
    ctx.r5.u64 = view;
    ctx.r4.u64 = f + 320;
    ctx.r3.u64 = f + 192;
    ctx.lr = 0x82067D74;
    sub_82041CA0(ctx, base);
  }
  // Transposed for the shader constants.
  for (u32 r = 0; r < 4; ++r)
    for (u32 c = 0; c < 4; ++c) gw_store32(f + 256 + (c * 4 + r) * 4, gw_load32(f + 192 + (r * 4 + c) * 4));
  ctx.r7.u64 = 0x8000000000000000ull;
  ctx.r6.u64 = 4;
  ctx.r5.u64 = f + 256;
  ctx.r4.u64 = 0;
  ctx.r3.u64 = gw_load32(A - 11632);
  ctx.lr = 0x82067E14;
  sub_820A6438(ctx, base);
  ctx.r4.u64 = 1;
  ctx.r3.u64 = gw_load32(A - 11632);
  ctx.lr = 0x82067E20;
  sub_8209C1B8(ctx, base);
  ctx.r1.u64 = sp;
  ctx.lr = lr;
}

// ---------------------------------------------------------------------------
// The second line style (guest 0x8205EF18): r3 and r4 the ends, r5 the
// colour, f1 the thickness. Four vertices from the stream at 0x821A91D8 and
// eight from 0x821A99E8 (the game's allocator 0x82061750, guest), written
// big-endian into the game's vertex chunk as the original. As 0x82060CC0,
// the arithmetic follows the original instruction by instruction (fN_k:
// the k-th value of its register fN), the calls with the registers it
// passes; the checker confirms it bit for bit. The kind 34 enemies'
// outlines (0x82061A30) draw with it.
extern "C" void sub_8205EF18(PPCContext& ctx, uint8_t* base) {
  const double width = ctx.f1.f64;
  const u32 sp = ctx.r1.u32;
  gw_store32(sp - 192, sp);  // the original's frame (its back chain)
  ctx.r1.u64 = sp - 192;
  u32 r10, r11, r28, r29, r30, r31;
  r11 = (u32)-2112159744;
  r29 = ctx.r3.u32;
  r28 = ctx.r4.u32;
  ctx.r3.u64 = (u32)(r11 + -28200);
  ctx.r4.u64 = (u32)4;
  r30 = ctx.r5.u32;
  ctx.lr = 0x8205EF4C;
  sub_82061750(ctx, base);
  const double f1_1 = ctx.f1.f64;
  r11 = (u32)-2112159744;
  r31 = ctx.r3.u32;
  r11 = (u32)(r11 + -26136);
  ctx.r4.u64 = (u32)8;
  ctx.r3.u64 = r11;
  ctx.lr = 0x8205EF64;
  ctx.f1.f64 = f1_1;
  sub_82061750(ctx, base);
  const double f1_2 = ctx.f1.f64;
  r11 = (u32)-2112225280;
  const float f12_1 = ldf(r30 + 12);
  r11 = (u32)(r11 + 7624);
  const float f11_1 = ldf(r30 + 0);
  const float f10_1 = ldf(r30 + 4);
  const float f11_2 = f12_1 * f11_1;
  const float f9_1 = ldf(r30 + 8);
  const float f8_1 = f10_1 * f12_1;
  const float f7_1 = ldf(r29 + 0);
  const float f9_2 = f9_1 * f12_1;
  const float f5_1 = ldf(r28 + 0);
  const float f0_1 = ldf(r11 + 32);
  r11 = (u32)-2113929216;
  const float f31_1 = f5_1 - f7_1;
  const float f6_1 = ldf(r29 + 4);
  const float f4_1 = ldf(r28 + 4);
  const float f3_1 = ldf(r29 + 8);
  const float f30_1 = f4_1 - f6_1;
  const float f2_1 = ldf(r28 + 8);
  const float f1_3 = ldf(r11 + 3208);
  r11 = (u32)-2113929216;
  const float f29_1 = f2_1 - f3_1;
  const float f13_1 = ldf(r11 + 1732);
  r11 = (u32)-2113929216;
  const float f10_2 = ldf(r11 + 10024);
  r11 = (u32)-2113929216;
  const float f12_2 = f11_2 * f10_2;
  const float f11_3 = f8_1 * f10_2;
  const float f10_3 = f9_2 * f10_2;
  const double f9_3 = (double)f31_1 * (double)f31_1;
  const double f8_2 = (double)f30_1 * (double)f30_1;
  const double f27_1 = ((double)f29_1 * (double)f29_1 + f9_3);
  const double f26_1 = f9_3 + f8_2;
  const double f9_4 = ldd(r11 + 10080);
  r11 = (u32)(r31 + 32);
  const double f8_3 = f27_1 + f8_2;
  const double f27_2 = f26_1 + f9_4;
  const double f8_4 = f8_3 + f9_4;
  const float f9_5 = (float)(1.0 / __builtin_sqrt(f27_2));
  const float f8_5 = (float)(1.0 / __builtin_sqrt(f8_4));
  const double f28_1 = (double)f8_5 * width;
  const double f8_6 = (double)f9_5 * (double)f31_1;
  const double f9_6 = (double)f9_5 * (double)f30_1;
  const double f31_2 = f28_1 * (double)f31_1;
  const double f8_7 = -f8_6;
  const double f30_2 = f28_1 * (double)f30_1;
  const double f29_2 = f28_1 * (double)f29_1;
  const double f28_2 = -((double)f0_1 * f9_6 + -(double)f7_1);
  const double f27_3 = ((double)f0_1 * f9_6 + (double)f7_1);
  const double f26_2 = -((double)f0_1 * f8_7 + -(double)f6_1);
  const float f28_3 = (float)f28_2;
  stf(r31 + 0, f28_3);
  const float f28_4 = (float)f27_3;
  const float f27_4 = (float)f26_2;
  stf(r31 + 4, f27_4);
  stf(r31 + 8, f3_1);
  const double f27_5 = ((double)f0_1 * f9_6 + (double)f5_1);
  stf(r31 + 12, f1_3);
  stf(r31 + 16, f12_2);
  stf(r31 + 20, f11_3);
  stf(r31 + 24, f10_3);
  stf(r31 + 28, f13_1);
  stf(r11 + 0, f28_4);
  const double f28_5 = ((double)f0_1 * f8_7 + (double)f6_1);
  const float f28_6 = (float)f28_5;
  stf(r11 + 4, f28_6);
  stf(r11 + 8, f3_1);
  stf(r11 + 12, f13_1);
  const double f28_7 = ((double)f0_1 * f8_7 + (double)f4_1);
  stf(r11 + 16, f12_2);
  const double f26_3 = -((double)f0_1 * f9_6 + -(double)f5_1);
  stf(r11 + 20, f11_3);
  const double f25_1 = -((double)f0_1 * f8_7 + -(double)f4_1);
  stf(r11 + 24, f10_3);
  const float f27_6 = (float)f27_5;
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  const double f24_1 = -((double)f0_1 * f9_6 + -(double)f7_1);
  ctx.r10.u64 = (u32)-2113929216;
  const double f23_1 = -((double)f0_1 * f8_7 + -(double)f6_1);
  stf(r11 + 0, f27_6);
  const float f28_8 = (float)f28_7;
  stf(r11 + 4, f28_8);
  stf(r11 + 8, f2_1);
  const float f26_4 = (float)f26_3;
  stf(r11 + 12, f13_1);
  const float f25_2 = (float)f25_1;
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  const float f28_9 = (float)f24_1;
  stf(r11 + 28, f13_1);
  const float f27_7 = (float)f23_1;
  stf(r11 + 32, f26_4);
  const double f24_2 = ((double)f0_1 * f8_7 + (double)f6_1);
  stf(r11 + 36, f25_2);
  const double f25_3 = ((double)f0_1 * f9_6 + (double)f7_1);
  stf(r11 + 40, f2_1);
  const double f23_2 = -((double)f0_1 * f29_2 + -(double)f3_1);
  stf(r11 + 44, f1_3);
  stf(r11 + 48, f12_2);
  stf(r11 + 52, f11_3);
  stf(r11 + 56, f10_3);
  stf(r11 + 60, f13_1);
  r11 = (u32)(ctx.r3.u32 + 32);
  stf(ctx.r3.u32 + 0, f28_9);
  const double f28_10 = ((double)f0_1 * f9_6 + (double)f7_1);
  stf(ctx.r3.u32 + 4, f27_7);
  const double f27_8 = ((double)f0_1 * f8_7 + (double)f6_1);
  stf(ctx.r3.u32 + 8, f3_1);
  const double f24_3 = -((double)f0_1 * f30_2 + -f24_2);
  stf(ctx.r3.u32 + 12, f1_3);
  const double f25_4 = -((double)f0_1 * f31_2 + -f25_3);
  stf(ctx.r3.u32 + 16, f12_2);
  const double f7_2 = -((double)f0_1 * f9_6 + -(double)f7_1);
  stf(ctx.r3.u32 + 20, f11_3);
  stf(ctx.r3.u32 + 24, f10_3);
  stf(ctx.r3.u32 + 28, f13_1);
  const float f26_5 = (float)f28_10;
  stf(r11 + 0, f26_5);
  const float f27_9 = (float)f27_8;
  stf(r11 + 4, f27_9);
  stf(r11 + 8, f3_1);
  const float f26_6 = (float)f24_3;
  stf(r11 + 12, f13_1);
  const float f25_5 = (float)f25_4;
  stf(r11 + 16, f12_2);
  const float f27_10 = (float)f23_2;
  stf(r11 + 20, f11_3);
  const double f7_3 = -((double)f0_1 * f31_2 + -f7_2);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  const float f28_11 = ldf(ctx.r10.u32 + 1740);
  stf(r11 + 0, f25_5);
  stf(r11 + 4, f26_6);
  stf(r11 + 8, f27_10);
  stf(r11 + 12, f13_1);
  const float f7_4 = (float)f7_3;
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f28_11);
  r11 = (u32)(r11 + 32);
  stf(r11 + 0, f7_4);
  const double f7_5 = -((double)f0_1 * f8_7 + -(double)f6_1);
  const double f6_2 = -((double)f0_1 * f29_2 + -(double)f3_1);
  const double f7_6 = -((double)f0_1 * f30_2 + -f7_5);
  const float f7_7 = (float)f7_6;
  stf(r11 + 4, f7_7);
  const float f6_3 = (float)f6_2;
  stf(r11 + 8, f6_3);
  const double f6_4 = -((double)f0_1 * f9_6 + -(double)f5_1);
  stf(r11 + 12, f1_3);
  stf(r11 + 16, f12_2);
  const double f3_2 = -((double)f0_1 * f8_7 + -(double)f4_1);
  stf(r11 + 20, f11_3);
  const double f27_11 = ((double)f0_1 * f9_6 + (double)f5_1);
  stf(r11 + 24, f10_3);
  const double f26_7 = ((double)f0_1 * f8_7 + (double)f4_1);
  stf(r11 + 28, f28_11);
  r11 = (u32)(r11 + 32);
  const double f7_8 = f9_6 + f31_2;
  const float f6_5 = (float)f6_4;
  stf(r11 + 0, f6_5);
  const float f3_3 = (float)f3_2;
  stf(r11 + 4, f3_3);
  stf(r11 + 8, f2_1);
  const float f27_12 = (float)f27_11;
  stf(r11 + 12, f1_3);
  const float f26_8 = (float)f26_7;
  stf(r11 + 16, f12_2);
  const double f6_6 = f8_7 + f30_2;
  stf(r11 + 20, f11_3);
  const double f7_9 = (f7_8 * (double)f0_1 + (double)f5_1);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  stf(r11 + 0, f27_12);
  stf(r11 + 4, f26_8);
  stf(r11 + 8, f2_1);
  stf(r11 + 12, f13_1);
  const float f7_10 = (float)f7_9;
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f13_1);
  r11 = (u32)(r11 + 32);
  stf(r11 + 0, f7_10);
  const double f7_11 = (f6_6 * (double)f0_1 + (double)f4_1);
  const double f6_7 = ((double)f0_1 * f29_2 + (double)f2_1);
  const float f7_12 = (float)f7_11;
  stf(r11 + 4, f7_12);
  const float f7_13 = (float)f6_7;
  stf(r11 + 8, f7_13);
  stf(r11 + 12, f13_1);
  const double f13_2 = -((double)f0_1 * f9_6 + -(double)f5_1);
  const double f9_7 = -((double)f0_1 * f8_7 + -(double)f4_1);
  stf(r11 + 16, f12_2);
  stf(r11 + 20, f11_3);
  stf(r11 + 24, f10_3);
  stf(r11 + 28, f28_11);
  const double f13_3 = ((double)f0_1 * f31_2 + f13_2);
  const double f9_8 = ((double)f0_1 * f30_2 + f9_7);
  const double f0_2 = ((double)f0_1 * f29_2 + (double)f2_1);
  const float f13_4 = (float)f13_3;
  stf(r11 + 32, f13_4);
  const float f9_9 = (float)f9_8;
  stf(r11 + 36, f9_9);
  const float f0_3 = (float)f0_2;
  stf(r11 + 40, f0_3);
  stf(r11 + 44, f1_3);
  stf(r11 + 48, f12_2);
  stf(r11 + 52, f11_3);
  stf(r11 + 56, f10_3);
  stf(r11 + 60, f28_11);
  (void)r10;
  ctx.r1.u64 = sp;
  ctx.lr = 0x8205F2D0;  // as the original leaves it (its register-restore call)
}

// ---------------------------------------------------------------------------
// The world update (guest 0x820366A0): the game's per-frame update in play
// (timers, spawning, every entity's Tick 0x820453E0 (native, above), the
// players, the grid's and particles' updates, sounds...), 1,367
// instructions calling 40 functions. This is the generated code itself,
// operation for operation, with the guest's volatile registers (r0, r3-r12,
// f0-f13) in locals instead of the context. Two dataflow passes over its
// statements (scripted from the generated code) place the copies: a local
// assigned since the last call is written back to the context before the
// next call or the return, and one read before being assigned is loaded
// from it at entry and after a call, so every callee and the caller see
// exactly the registers the original leaves. What it saves is the context
// traffic: every register access was a memory access the compiler had to
// keep, flushed again at every label.
extern "C" void sub_82030058(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030168(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820304C8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030A98(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030C58(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82033610(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82033638(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82034408(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82034588(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82034CA0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820359E0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82036358(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820364D0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8203BC08(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8203E7E8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82043368(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820453E0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820476F0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82048638(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820499E0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204A9C0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204AD38(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204B770(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204D410(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204EBE8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204F3B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204F3F8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204FAD8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82051448(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820523B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205C8F8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205CA98(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205E068(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205E1E0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82067388(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82067390(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820787F8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82078970(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82090DA8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820E8130(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8212EA80(PPCContext& ctx, uint8_t* base);
#ifdef GW_BIG_NATIVE  // (make BIG_NATIVE=y): the generated code with registers in locals; no faster on the PC, part of it untested
extern "C" void sub_820366A0(PPCContext& ctx, uint8_t* base) {
	PPCRegister v_f1{}; PPCRegister v_f2{}; PPCRegister v_f3{}; PPCRegister v_f4{}; PPCRegister v_f5{}; PPCRegister v_f6{}; PPCRegister v_f7{}; PPCRegister v_f8{}; PPCRegister v_f9{}; PPCRegister v_f10{}; PPCRegister v_f11{}; PPCRegister v_f12{}; PPCRegister v_f13{}; PPCRegister v_r3{}; PPCRegister v_r4{}; PPCRegister v_r5{}; PPCRegister v_r6{}; PPCRegister v_r7{}; PPCRegister v_r8{}; PPCRegister v_r9{}; PPCRegister v_r10{}; PPCRegister v_r12 = ctx.r12;

	REX_FUNC_PROLOGUE();
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r0{};
	PPCRegister r11{};
	PPCRegister r12 = v_r12;
	PPCRegister r19{};
	PPCRegister r20{};
	PPCRegister r21{};
	PPCRegister r22{};
	PPCRegister r23{};
	PPCRegister r24{};
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f0{};
	PPCRegister f23{};
	PPCRegister f24{};
	PPCRegister f25{};
	PPCRegister f26{};
	PPCRegister f27{};
	PPCRegister f28{};
	PPCRegister f29{};
	PPCRegister f30{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	r12.u64 = ctx.lr;
	// bl 0x8212e914
	ctx.lr = 0x820366A8;
	// addi r12,r1,-112
	r12.s64 = ctx.r1.s64 + -112;
	// bl 0x8212e884
	ctx.lr = 0x820366B0;
	// stwu r1,-640(r1)
	ea = -640 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// bl 0x82090da8
	ctx.lr = 0x820366B8;
	sub_82090DA8(ctx, base);
	// bl 0x82034588
	ctx.lr = 0x820366BC;
	sub_82034588(ctx, base);
	v_r3 = ctx.r3;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// lis r11,-32230
	r11.s64 = -2112225280;
	// addi r22,r11,-27536
	r22.s64 = r11.s64 + -27536;
	// bne cr6,0x82037b0c
	if (!cr6.eq) goto loc_82037B0C;
	// bl 0x8205e1e0
	ctx.lr = 0x820366D4;
	sub_8205E1E0(ctx, base);
	// li r20,0
	r20.s64 = 0;
	// lis r10,-32230
	v_r10.s64 = -2112225280;
	// mr r11,r20
	r11.u64 = r20.u64;
	// stw r11,-11644(r10)
	REX_STORE_U32(v_r10.u32 + -11644, r11.u32);
	// bl 0x8203e7e8
	ctx.lr = 0x820366E8;
	ctx.r10 = v_r10;
	sub_8203E7E8(ctx, base);
	// lis r19,-32230
	r19.s64 = -2112225280;
	// lfs f0,14840(r22)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r22.u32 + 14840);
	f0.f64 = double(temp.f32);
	// fctiwz f12,f0
	v_f12.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// addi r10,r1,112
	v_r10.s64 = ctx.r1.s64 + 112;
	// lfs f13,-11648(r19)
	temp.u32 = REX_LOAD_U32(r19.u32 + -11648);
	v_f13.f64 = double(temp.f32);
	// fadds f0,f13,f0
	f0.f64 = double(float(v_f13.f64 + f0.f64));
	// stfs f0,14840(r22)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r22.u32 + 14840, temp.u32);
	// lwz r11,15844(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 15844);
	// stfiwx f12,0,r10
	REX_STORE_U32(v_r10.u32, v_f12.u32);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x8203674c
	if (!cr6.eq) goto loc_8203674C;
	// bl 0x82033610
	ctx.lr = 0x82036718;
	ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r10 = v_r10;
	sub_82033610(ctx, base);
	v_f12 = ctx.f12; v_f13 = ctx.f13; v_r3 = ctx.r3; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x8203674c
	if (cr6.eq) goto loc_8203674C;
	// addi r11,r1,80
	r11.s64 = ctx.r1.s64 + 80;
	// lfs f0,14840(r22)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r22.u32 + 14840);
	f0.f64 = double(temp.f32);
	// fctiwz f0,f0
	f0.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// lwz r10,112(r1)
	v_r10.u64 = REX_LOAD_U32(ctx.r1.u32 + 112);
	// stfiwx f0,0,r11
	REX_STORE_U32(r11.u32, f0.u32);
	// lwz r11,80(r1)
	r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);
	// cmpw cr6,r10,r11
	cr6.compare<int32_t>(v_r10.s32, r11.s32, xer);
	// beq cr6,0x8203674c
	if (cr6.eq) goto loc_8203674C;
	// li r3,75
	v_r3.s64 = 75;
	// bl 0x820787f8
	ctx.lr = 0x8203674C;
	ctx.r3 = v_r3; ctx.r10 = v_r10;
	sub_820787F8(ctx, base);
	v_f12 = ctx.f12; v_f13 = ctx.f13; v_r10 = ctx.r10;
loc_8203674C:
	// bl 0x82043368
	ctx.lr = 0x82036750;
	ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r10 = v_r10;
	sub_82043368(ctx, base);
	// lis r11,-32230
	r11.s64 = -2112225280;
	// addi r3,r11,-11652
	v_r3.s64 = r11.s64 + -11652;
	// bl 0x820359e0
	ctx.lr = 0x8203675C;
	ctx.r3 = v_r3;
	sub_820359E0(ctx, base);
	// lis r11,-32232
	r11.s64 = -2112356352;
	// addi r21,r11,5032
	r21.s64 = r11.s64 + 5032;
	// lwz r31,4(r21)
	r31.u64 = REX_LOAD_U32(r21.u32 + 4);
	// lwz r3,0(r31)
	v_r3.u64 = REX_LOAD_U32(r31.u32 + 0);
	// cmpwi cr6,r3,0
	cr6.compare<int32_t>(v_r3.s32, 0, xer);
	// beq cr6,0x82036798
	if (cr6.eq) goto loc_82036798;
loc_82036774:
	
	// lwz r11,36(r3)
	r11.u64 = REX_LOAD_U32(v_r3.u32 + 36);
	// clrlwi r11,r11,31
	r11.u64 = r11.u32 & 0x1;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x82036788
	if (!cr6.eq) goto loc_82036788;
	// bl 0x820453e0
	ctx.lr = 0x82036788;
	ctx.r3 = v_r3;
	sub_820453E0(ctx, base);
loc_82036788:
	// lwz r31,4(r31)
	r31.u64 = REX_LOAD_U32(r31.u32 + 4);
	// lwz r3,0(r31)
	v_r3.u64 = REX_LOAD_U32(r31.u32 + 0);
	// cmpwi cr6,r3,0
	cr6.compare<int32_t>(v_r3.s32, 0, xer);
	// bne cr6,0x82036774
	if (!cr6.eq) goto loc_82036774;
loc_82036798:
	// lis r11,-32232
	r11.s64 = -2112356352;
	// addi r11,r11,5008
	r11.s64 = r11.s64 + 5008;
	// lwz r31,4(r11)
	r31.u64 = REX_LOAD_U32(r11.u32 + 4);
	// lwz r3,0(r31)
	v_r3.u64 = REX_LOAD_U32(r31.u32 + 0);
	// cmpwi cr6,r3,0
	cr6.compare<int32_t>(v_r3.s32, 0, xer);
	// beq cr6,0x820367d4
	if (cr6.eq) goto loc_820367D4;
loc_820367B0:
	
	// lwz r11,36(r3)
	r11.u64 = REX_LOAD_U32(v_r3.u32 + 36);
	// clrlwi r11,r11,30
	r11.u64 = r11.u32 & 0x3;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x820367c4
	if (!cr6.eq) goto loc_820367C4;
	// bl 0x820453e0
	ctx.lr = 0x820367C4;
	ctx.r3 = v_r3;
	sub_820453E0(ctx, base);
loc_820367C4:
	// lwz r31,4(r31)
	r31.u64 = REX_LOAD_U32(r31.u32 + 4);
	// lwz r3,0(r31)
	v_r3.u64 = REX_LOAD_U32(r31.u32 + 0);
	// cmpwi cr6,r3,0
	cr6.compare<int32_t>(v_r3.s32, 0, xer);
	// bne cr6,0x820367b0
	if (!cr6.eq) goto loc_820367B0;
loc_820367D4:
	// bl 0x820523b0
	ctx.lr = 0x820367D8;
	ctx.r3 = v_r3;
	sub_820523B0(ctx, base);
	// bl 0x820476f0
	ctx.lr = 0x820367DC;
	sub_820476F0(ctx, base);
	// bl 0x8204d410
	ctx.lr = 0x820367E0;
	sub_8204D410(ctx, base);
	// bl 0x8204b770
	ctx.lr = 0x820367E4;
	sub_8204B770(ctx, base);
	v_f13 = ctx.f13;
	// lwz r11,15348(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 15348);
	// mr r10,r20
	v_r10.u64 = r20.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f27,3440(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 3440);
	f27.f64 = double(temp.f32);
	// ble cr6,0x8203682c
	if (!cr6.gt) goto loc_8203682C;
	// addi r11,r22,15444
	r11.s64 = r22.s64 + 15444;
loc_82036800:
	
	// lfs f0,-4(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + -4);
	f0.f64 = double(temp.f32);
	// addi r10,r10,1
	v_r10.s64 = v_r10.s64 + 1;
	// lfs f13,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	v_f13.f64 = double(temp.f32);
	// fmuls f0,f0,f27
	f0.f64 = double(float(f0.f64 * f27.f64));
	// fmuls f13,f13,f27
	v_f13.f64 = double(float(v_f13.f64 * f27.f64));
	// stfs f0,-4(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + -4, temp.u32);
	// stfs f13,0(r11)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(r11.u32 + 0, temp.u32);
	// lwz r9,15348(r22)
	v_r9.u64 = REX_LOAD_U32(r22.u32 + 15348);
	// addi r11,r11,120
	r11.s64 = r11.s64 + 120;
	// cmpw cr6,r10,r9
	cr6.compare<int32_t>(v_r10.s32, v_r9.s32, xer);
	// blt cr6,0x82036800
	if (cr6.lt) goto loc_82036800;
loc_8203682C:
	// lwz r10,4(r21)
	v_r10.u64 = REX_LOAD_U32(r21.u32 + 4);
	// mr r9,r20
	v_r9.u64 = r20.u64;
	// lwz r11,0(r10)
	r11.u64 = REX_LOAD_U32(v_r10.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// beq cr6,0x82036864
	if (cr6.eq) goto loc_82036864;
loc_82036840:
	
	// lwz r11,36(r11)
	r11.u64 = REX_LOAD_U32(r11.u32 + 36);
	// andi. r11,r11,17
	r11.u64 = r11.u64 & 17;
	cr0.compare<int32_t>(r11.s32, 0, xer);
	// cmpwi cr6,r11,16
	cr6.compare<int32_t>(r11.s32, 16, xer);
	// bne cr6,0x82036854
	if (!cr6.eq) goto loc_82036854;
	// li r9,1
	v_r9.s64 = 1;
loc_82036854:
	// lwz r10,4(r10)
	v_r10.u64 = REX_LOAD_U32(v_r10.u32 + 4);
	// lwz r11,0(r10)
	r11.u64 = REX_LOAD_U32(v_r10.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x82036840
	if (!cr6.eq) goto loc_82036840;
loc_82036864:
	// lis r10,-32230
	v_r10.s64 = -2112225280;
	// clrlwi r11,r9,24
	r11.u64 = v_r9.u32 & 0xFF;
	// lbz r8,-11312(r10)
	v_r8.u64 = REX_LOAD_U8(v_r10.u32 + -11312);
	// stb r9,-11312(r10)
	REX_STORE_U8(v_r10.u32 + -11312, v_r9.u8);
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// xor r10,r8,r11
	v_r10.u64 = v_r8.u64 ^ r11.u64;
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// and r11,r10,r11
	r11.u64 = v_r10.u64 & r11.u64;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// clrlwi r11,r11,24
	r11.u64 = r11.u32 & 0xFF;
	// lfs f30,1732(r9)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r9.u32 + 1732);
	f30.f64 = double(temp.f32);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// lfs f31,1788(r8)
	temp.u32 = REX_LOAD_U32(v_r8.u32 + 1788);
	f31.f64 = double(temp.f32);
	// lis r11,-32230
	r11.s64 = -2112225280;
	// lfs f24,1740(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 1740);
	f24.f64 = double(temp.f32);
	// addi r23,r11,7720
	r23.s64 = r11.s64 + 7720;
	// beq cr6,0x820369b0
	if (cr6.eq) goto loc_820369B0;
	// lbz r11,15877(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 15877);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x820369b0
	if (!cr6.eq) goto loc_820369B0;
	// bl 0x8204f3f8
	ctx.lr = 0x820368B8;
	ctx.f13 = v_f13; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8204F3F8(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(v_r3.u32, 0, xer);
	// beq cr6,0x820369b0
	if (cr6.eq) goto loc_820369B0;
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// lfs f0,8(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r3.u32 + 8);
	f0.f64 = double(temp.f32);
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lfs f13,4(r3)
	temp.u32 = REX_LOAD_U32(v_r3.u32 + 4);
	v_f13.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f13,224(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 224, temp.u32);
	// stfs f24,232(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 232, temp.u32);
	// li r30,40
	r30.s64 = 40;
	// stfs f30,236(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 236, temp.u32);
	// lfs f26,1776(r9)
	temp.u32 = REX_LOAD_U32(v_r9.u32 + 1776);
	f26.f64 = double(temp.f32);
	// lfs f28,3436(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3436);
	f28.f64 = double(temp.f32);
	// lfs f29,3432(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3432);
	f29.f64 = double(temp.f32);
	// stfs f0,228(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 228, temp.u32);
loc_820368F4:
	
	// li r31,64
	r31.s64 = 64;
loc_820368F8:
	
	// addi r3,r1,112
	v_r3.s64 = ctx.r1.s64 + 112;
	// stfs f31,240(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 240, temp.u32);
	// mr r4,r23
	v_r4.u64 = r23.u64;
	// stfs f31,244(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 244, temp.u32);
	// stfs f29,248(r1)
	temp.f32 = float(f29.f64);
	REX_STORE_U32(ctx.r1.u32 + 248, temp.u32);
	// fmr f1,f28
	v_f1.f64 = f28.f64;
	// stfs f30,252(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 252, temp.u32);
	// bl 0x82030168
	ctx.lr = 0x82036918;
	ctx.f1 = v_f1; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82030168(ctx, base);
	v_r3 = ctx.r3;
	// mr r11,r3
	r11.u64 = v_r3.u64;
	// addi r5,r1,240
	v_r5.s64 = ctx.r1.s64 + 240;
	// addi r4,r1,352
	v_r4.s64 = ctx.r1.s64 + 352;
	// addi r3,r1,224
	v_r3.s64 = ctx.r1.s64 + 224;
	// lfs f0,12(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 12);
	f0.f64 = double(temp.f32);
	// lfs f12,8(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 8);
	v_f12.f64 = double(temp.f32);
	// fmuls f0,f0,f24
	f0.f64 = double(float(f0.f64 * f24.f64));
	// lfs f13,4(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 4);
	v_f13.f64 = double(temp.f32);
	// fmuls f12,f12,f27
	v_f12.f64 = double(float(v_f12.f64 * f27.f64));
	// lfs f11,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	v_f11.f64 = double(temp.f32);
	// stfs f11,352(r1)
	temp.f32 = float(v_f11.f64);
	REX_STORE_U32(ctx.r1.u32 + 352, temp.u32);
	// stfs f0,364(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 364, temp.u32);
	// stfs f13,356(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 356, temp.u32);
	// stfs f12,360(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 360, temp.u32);
	// bl 0x82067388
	ctx.lr = 0x82036954;
	ctx.f11 = v_f11; ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_82067388(ctx, base);
	v_f13 = ctx.f13; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// addi r31,r31,-1
	r31.s64 = r31.s64 + -1;
	// cmplwi cr6,r31,0
	cr6.compare<uint32_t>(r31.u32, 0, xer);
	// bne cr6,0x820368f8
	if (!cr6.eq) goto loc_820368F8;
	// li r31,8
	r31.s64 = 8;
loc_82036964:
	
	// addi r3,r1,432
	v_r3.s64 = ctx.r1.s64 + 432;
	// stfs f29,288(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f29.f64);
	REX_STORE_U32(ctx.r1.u32 + 288, temp.u32);
	// mr r4,r23
	v_r4.u64 = r23.u64;
	// stfs f31,292(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 292, temp.u32);
	// stfs f31,296(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 296, temp.u32);
	// addi r29,r1,288
	r29.s64 = ctx.r1.s64 + 288;
	// stfs f30,300(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 300, temp.u32);
	// fmr f1,f26
	v_f1.f64 = f26.f64;
	// bl 0x82030168
	ctx.lr = 0x82036988;
	ctx.f1 = v_f1; ctx.r3 = v_r3; ctx.r4 = v_r4;
	sub_82030168(ctx, base);
	v_r3 = ctx.r3;
	// mr r4,r3
	v_r4.u64 = v_r3.u64;
	// addi r3,r1,224
	v_r3.s64 = ctx.r1.s64 + 224;
	// mr r5,r29
	v_r5.u64 = r29.u64;
	// bl 0x82067388
	ctx.lr = 0x82036998;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_82067388(ctx, base);
	v_f13 = ctx.f13; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// addi r31,r31,-1
	r31.s64 = r31.s64 + -1;
	// cmplwi cr6,r31,0
	cr6.compare<uint32_t>(r31.u32, 0, xer);
	// bne cr6,0x82036964
	if (!cr6.eq) goto loc_82036964;
	// addi r30,r30,-1
	r30.s64 = r30.s64 + -1;
	// cmplwi cr6,r30,0
	cr6.compare<uint32_t>(r30.u32, 0, xer);
	// bne cr6,0x820368f4
	if (!cr6.eq) goto loc_820368F4;
loc_820369B0:
	// bl 0x82078970
	ctx.lr = 0x820369B4;
	ctx.f13 = v_f13; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82078970(ctx, base);
	// bl 0x82033610
	ctx.lr = 0x820369B8;
	sub_82033610(ctx, base);
	v_f1 = ctx.f1; v_f2 = ctx.f2; v_f3 = ctx.f3; v_f4 = ctx.f4; v_f5 = ctx.f5; v_f6 = ctx.f6; v_f7 = ctx.f7; v_f8 = ctx.f8; v_f9 = ctx.f9; v_f10 = ctx.f10; v_f11 = ctx.f11; v_f12 = ctx.f12; v_r3 = ctx.r3; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// lis r24,-32232
	r24.s64 = -2112356352;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lis r25,-32232
	r25.s64 = -2112356352;
	// lfs f23,1824(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 1824);
	f23.f64 = double(temp.f32);
	// beq cr6,0x82036f58
	if (cr6.eq) goto loc_82036F58;
	// lis r11,-32230
	r11.s64 = -2112225280;
	// addi r11,r11,-11628
	r11.s64 = r11.s64 + -11628;
	// lbz r11,32(r11)
	r11.u64 = REX_LOAD_U8(r11.u32 + 32);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82036f48
	if (cr6.eq) goto loc_82036F48;
	// lbz r11,15876(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 15876);
	// lfs f12,9652(r24)
	temp.u32 = REX_LOAD_U32(r24.u32 + 9652);
	v_f12.f64 = double(temp.f32);
	// lfs f13,9656(r25)
	temp.u32 = REX_LOAD_U32(r25.u32 + 9656);
	v_f13.f64 = double(temp.f32);
	// fmr f8,f12
	v_f8.f64 = v_f12.f64;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// fmr f7,f13
	v_f7.f64 = v_f13.f64;
	// lis r11,-32230
	r11.s64 = -2112225280;
	// fneg f6,f12
	v_f6.u64 = v_f12.u64 ^ 0x8000000000000000;
	// fneg f5,f13
	v_f5.u64 = v_f13.u64 ^ 0x8000000000000000;
	// stfs f8,88(r1)
	temp.f32 = float(v_f8.f64);
	REX_STORE_U32(ctx.r1.u32 + 88, temp.u32);
	// stfs f7,92(r1)
	temp.f32 = float(v_f7.f64);
	REX_STORE_U32(ctx.r1.u32 + 92, temp.u32);
	// addi r26,r11,-11416
	r26.s64 = r11.s64 + -11416;
	// stfs f6,96(r1)
	temp.f32 = float(v_f6.f64);
	REX_STORE_U32(ctx.r1.u32 + 96, temp.u32);
	// stfs f5,100(r1)
	temp.f32 = float(v_f5.f64);
	REX_STORE_U32(ctx.r1.u32 + 100, temp.u32);
	// beq cr6,0x82036cd0
	if (cr6.eq) goto loc_82036CD0;
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// mr r27,r20
	r27.u64 = r20.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x82036cd0
	if (!cr6.gt) goto loc_82036CD0;
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// addi r31,r22,14948
	r31.s64 = r22.s64 + 14948;
	// lfs f27,3428(r9)
	temp.u32 = REX_LOAD_U32(v_r9.u32 + 3428);
	f27.f64 = double(temp.f32);
	// lfs f28,3424(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3424);
	f28.f64 = double(temp.f32);
	// lfs f29,3420(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3420);
	f29.f64 = double(temp.f32);
loc_82036A50:
	
	// lwz r11,-32(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + -32);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// addi r11,r11,4
	r11.s64 = r11.s64 + 4;
	// bne cr6,0x82036a64
	if (!cr6.eq) goto loc_82036A64;
	// addi r11,r31,-20
	r11.s64 = r31.s64 + -20;
loc_82036A64:
	// lbz r10,15877(r22)
	v_r10.u64 = REX_LOAD_U8(r22.u32 + 15877);
	// lfs f0,0(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f0.f64 = double(temp.f32);
	// lfs f13,4(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 4);
	v_f13.f64 = double(temp.f32);
	// addi r30,r31,-4
	r30.s64 = r31.s64 + -4;
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(v_r10.u32, 0, xer);
	// addi r29,r31,4
	r29.s64 = r31.s64 + 4;
	// addi r28,r31,8
	r28.s64 = r31.s64 + 8;
	// beq cr6,0x82036b20
	if (cr6.eq) goto loc_82036B20;
	// lfs f12,-12(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -12);
	v_f12.f64 = double(temp.f32);
	// fsubs f0,f0,f12
	f0.f64 = double(float(f0.f64 - v_f12.f64));
	// lfs f11,-8(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -8);
	v_f11.f64 = double(temp.f32);
	// fsubs f13,f13,f11
	v_f13.f64 = double(float(v_f13.f64 - v_f11.f64));
	// fmuls f0,f0,f29
	f0.f64 = double(float(f0.f64 * f29.f64));
	// fmuls f13,f13,f29
	v_f13.f64 = double(float(v_f13.f64 * f29.f64));
	// fadds f0,f0,f12
	f0.f64 = double(float(f0.f64 + v_f12.f64));
	// stfs f0,-12(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + -12, temp.u32);
	// fadds f0,f13,f11
	f0.f64 = double(float(v_f13.f64 + v_f11.f64));
	// stfs f0,-8(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + -8, temp.u32);
	// lfs f13,-4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -4);
	v_f13.f64 = double(temp.f32);
	// lfs f0,-12(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -12);
	f0.f64 = double(temp.f32);
	// fsubs f0,f0,f13
	f0.f64 = double(float(f0.f64 - v_f13.f64));
	// lfs f12,-8(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -8);
	v_f12.f64 = double(temp.f32);
	// lfs f13,0(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 0);
	v_f13.f64 = double(temp.f32);
	// fsubs f13,f12,f13
	v_f13.f64 = double(float(v_f12.f64 - v_f13.f64));
	// lfs f12,-4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -4);
	v_f12.f64 = double(temp.f32);
	// lfs f11,0(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 0);
	v_f11.f64 = double(temp.f32);
	// fmuls f0,f0,f28
	f0.f64 = double(float(f0.f64 * f28.f64));
	// fmuls f13,f13,f28
	v_f13.f64 = double(float(v_f13.f64 * f28.f64));
	// fadds f0,f0,f12
	f0.f64 = double(float(f0.f64 + v_f12.f64));
	// stfs f0,-4(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + -4, temp.u32);
	// fadds f0,f13,f11
	f0.f64 = double(float(v_f13.f64 + v_f11.f64));
	// stfs f0,0(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + 0, temp.u32);
	// lfs f13,4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 4);
	v_f13.f64 = double(temp.f32);
	// lfs f0,-4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -4);
	f0.f64 = double(temp.f32);
	// fsubs f0,f0,f13
	f0.f64 = double(float(f0.f64 - v_f13.f64));
	// lfs f12,0(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 0);
	v_f12.f64 = double(temp.f32);
	// lfs f13,8(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 8);
	v_f13.f64 = double(temp.f32);
	// fsubs f13,f12,f13
	v_f13.f64 = double(float(v_f12.f64 - v_f13.f64));
	// lfs f12,4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 4);
	v_f12.f64 = double(temp.f32);
	// lfs f11,8(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 8);
	v_f11.f64 = double(temp.f32);
	// fmuls f0,f0,f27
	f0.f64 = double(float(f0.f64 * f27.f64));
	// fmuls f13,f13,f27
	v_f13.f64 = double(float(v_f13.f64 * f27.f64));
	// fadds f0,f0,f12
	f0.f64 = double(float(f0.f64 + v_f12.f64));
	// stfs f0,4(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + 4, temp.u32);
	// fadds f0,f13,f11
	f0.f64 = double(float(v_f13.f64 + v_f11.f64));
	// stfs f0,8(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + 8, temp.u32);
	// b 0x82036b38
	goto loc_82036B38;
loc_82036B20:
	// stfs f0,-12(r31)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + -12, temp.u32);
	// stfs f13,-8(r31)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(r31.u32 + -8, temp.u32);
	// stfs f0,-4(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + -4, temp.u32);
	// stfs f13,0(r31)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(r31.u32 + 0, temp.u32);
	// stfs f0,4(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + 4, temp.u32);
	// stfs f13,8(r31)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(r31.u32 + 8, temp.u32);
loc_82036B38:
	// lbz r11,32(r26)
	r11.u64 = REX_LOAD_U8(r26.u32 + 32);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82036c2c
	if (cr6.eq) goto loc_82036C2C;
	// lfs f0,-12(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + -12);
	f0.f64 = double(temp.f32);
	// addi r5,r1,416
	v_r5.s64 = ctx.r1.s64 + 416;
	// fadds f12,f0,f31
	v_f12.f64 = double(float(f0.f64 + f31.f64));
	// lfs f13,-8(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -8);
	v_f13.f64 = double(temp.f32);
	// fsubs f0,f0,f31
	f0.f64 = double(float(f0.f64 - f31.f64));
	// stfs f12,192(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 192, temp.u32);
	// stfs f0,184(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 184, temp.u32);
	// addi r4,r1,192
	v_r4.s64 = ctx.r1.s64 + 192;
	// addi r3,r1,184
	v_r3.s64 = ctx.r1.s64 + 184;
	// fadds f12,f13,f31
	v_f12.f64 = double(float(v_f13.f64 + f31.f64));
	// fsubs f0,f13,f31
	f0.f64 = double(float(v_f13.f64 - f31.f64));
	// stfs f30,416(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 416, temp.u32);
	// stfs f30,420(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 420, temp.u32);
	// stfs f24,424(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 424, temp.u32);
	// stfs f30,428(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 428, temp.u32);
	// stfs f12,196(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 196, temp.u32);
	// stfs f0,188(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 188, temp.u32);
	// bl 0x8205e068
	ctx.lr = 0x82036B8C;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f6 = v_f6; ctx.f7 = v_f7; ctx.f8 = v_f8; ctx.f9 = v_f9; ctx.f10 = v_f10; ctx.f11 = v_f11; ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8205E068(ctx, base);
	// lfs f0,0(r30)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r30.u32 + 0);
	f0.f64 = double(temp.f32);
	// addi r5,r1,320
	v_r5.s64 = ctx.r1.s64 + 320;
	// fadds f12,f0,f31
	v_f12.f64 = double(float(f0.f64 + f31.f64));
	// lfs f13,0(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 0);
	v_f13.f64 = double(temp.f32);
	// fsubs f0,f0,f31
	f0.f64 = double(float(f0.f64 - f31.f64));
	// stfs f12,152(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 152, temp.u32);
	// stfs f0,216(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 216, temp.u32);
	// addi r4,r1,152
	v_r4.s64 = ctx.r1.s64 + 152;
	// addi r3,r1,216
	v_r3.s64 = ctx.r1.s64 + 216;
	// fadds f12,f13,f31
	v_f12.f64 = double(float(v_f13.f64 + f31.f64));
	// fsubs f0,f13,f31
	f0.f64 = double(float(v_f13.f64 - f31.f64));
	// stfs f30,320(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 320, temp.u32);
	// stfs f30,324(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 324, temp.u32);
	// stfs f24,328(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 328, temp.u32);
	// stfs f30,332(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 332, temp.u32);
	// stfs f12,156(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 156, temp.u32);
	// stfs f0,220(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 220, temp.u32);
	// bl 0x8205e068
	ctx.lr = 0x82036BD4;
	ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_8205E068(ctx, base);
	// lfs f0,0(r29)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r29.u32 + 0);
	f0.f64 = double(temp.f32);
	// addi r5,r1,384
	v_r5.s64 = ctx.r1.s64 + 384;
	// fadds f12,f0,f31
	v_f12.f64 = double(float(f0.f64 + f31.f64));
	// lfs f13,0(r28)
	temp.u32 = REX_LOAD_U32(r28.u32 + 0);
	v_f13.f64 = double(temp.f32);
	// fsubs f0,f0,f31
	f0.f64 = double(float(f0.f64 - f31.f64));
	// stfs f12,168(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 168, temp.u32);
	// stfs f0,200(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 200, temp.u32);
	// addi r4,r1,168
	v_r4.s64 = ctx.r1.s64 + 168;
	// addi r3,r1,200
	v_r3.s64 = ctx.r1.s64 + 200;
	// fadds f12,f13,f31
	v_f12.f64 = double(float(v_f13.f64 + f31.f64));
	// fsubs f0,f13,f31
	f0.f64 = double(float(v_f13.f64 - f31.f64));
	// stfs f30,384(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 384, temp.u32);
	// stfs f30,388(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 388, temp.u32);
	// stfs f24,392(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 392, temp.u32);
	// stfs f30,396(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 396, temp.u32);
	// stfs f12,172(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 172, temp.u32);
	// stfs f0,204(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 204, temp.u32);
	// bl 0x8205e068
	ctx.lr = 0x82036C1C;
	ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_8205E068(ctx, base);
	v_r9 = ctx.r9;
	// lfs f7,92(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 92);
	v_f7.f64 = double(temp.f32);
	// lfs f8,88(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 88);
	v_f8.f64 = double(temp.f32);
	// lfs f5,100(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 100);
	v_f5.f64 = double(temp.f32);
	// lfs f6,96(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	v_f6.f64 = double(temp.f32);
loc_82036C2C:
	// lfs f0,-12(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + -12);
	f0.f64 = double(temp.f32);
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// lfs f13,-8(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -8);
	v_f13.f64 = double(temp.f32);
	// fsubs f4,f8,f0
	v_f4.f64 = double(float(v_f8.f64 - f0.f64));
	// fsubs f3,f7,f13
	v_f3.f64 = double(float(v_f7.f64 - v_f13.f64));
	// lfs f12,0(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 0);
	v_f12.f64 = double(temp.f32);
	// fsubs f2,f0,f6
	v_f2.f64 = double(float(f0.f64 - v_f6.f64));
	// lfs f11,0(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 0);
	v_f11.f64 = double(temp.f32);
	// fsubs f1,f13,f5
	v_f1.f64 = double(float(v_f13.f64 - v_f5.f64));
	// lfs f10,0(r29)
	temp.u32 = REX_LOAD_U32(r29.u32 + 0);
	v_f10.f64 = double(temp.f32);
	// lfs f9,0(r28)
	temp.u32 = REX_LOAD_U32(r28.u32 + 0);
	v_f9.f64 = double(temp.f32);
	// addi r27,r27,1
	r27.s64 = r27.s64 + 1;
	// addi r31,r31,108
	r31.s64 = r31.s64 + 108;
	// cmpw cr6,r27,r11
	cr6.compare<int32_t>(r27.s32, r11.s32, xer);
	// fsel f8,f4,f0,f8
	v_f8.f64 = v_f4.f64 >= 0.0 ? f0.f64 : v_f8.f64;
	// fsel f7,f3,f13,f7
	v_f7.f64 = v_f3.f64 >= 0.0 ? v_f13.f64 : v_f7.f64;
	// fsel f0,f2,f0,f6
	f0.f64 = v_f2.f64 >= 0.0 ? f0.f64 : v_f6.f64;
	// fsel f13,f1,f13,f5
	v_f13.f64 = v_f1.f64 >= 0.0 ? v_f13.f64 : v_f5.f64;
	// fsubs f6,f8,f11
	v_f6.f64 = double(float(v_f8.f64 - v_f11.f64));
	// fsubs f5,f7,f12
	v_f5.f64 = double(float(v_f7.f64 - v_f12.f64));
	// fsubs f4,f11,f0
	v_f4.f64 = double(float(v_f11.f64 - f0.f64));
	// fsubs f3,f12,f13
	v_f3.f64 = double(float(v_f12.f64 - v_f13.f64));
	// fsel f8,f6,f11,f8
	v_f8.f64 = v_f6.f64 >= 0.0 ? v_f11.f64 : v_f8.f64;
	// fsel f7,f5,f12,f7
	v_f7.f64 = v_f5.f64 >= 0.0 ? v_f12.f64 : v_f7.f64;
	// fsel f0,f4,f11,f0
	f0.f64 = v_f4.f64 >= 0.0 ? v_f11.f64 : f0.f64;
	// fsel f13,f3,f12,f13
	v_f13.f64 = v_f3.f64 >= 0.0 ? v_f12.f64 : v_f13.f64;
	// fsubs f12,f8,f10
	v_f12.f64 = double(float(v_f8.f64 - v_f10.f64));
	// fsubs f11,f7,f9
	v_f11.f64 = double(float(v_f7.f64 - v_f9.f64));
	// fsubs f6,f10,f0
	v_f6.f64 = double(float(v_f10.f64 - f0.f64));
	// fsubs f5,f9,f13
	v_f5.f64 = double(float(v_f9.f64 - v_f13.f64));
	// fsel f8,f12,f10,f8
	v_f8.f64 = v_f12.f64 >= 0.0 ? v_f10.f64 : v_f8.f64;
	// stfs f8,88(r1)
	temp.f32 = float(v_f8.f64);
	REX_STORE_U32(ctx.r1.u32 + 88, temp.u32);
	// fsel f7,f11,f9,f7
	v_f7.f64 = v_f11.f64 >= 0.0 ? v_f9.f64 : v_f7.f64;
	// stfs f7,92(r1)
	temp.f32 = float(v_f7.f64);
	REX_STORE_U32(ctx.r1.u32 + 92, temp.u32);
	// fsel f6,f6,f10,f0
	v_f6.f64 = v_f6.f64 >= 0.0 ? v_f10.f64 : f0.f64;
	// stfs f6,96(r1)
	temp.f32 = float(v_f6.f64);
	REX_STORE_U32(ctx.r1.u32 + 96, temp.u32);
	// fsel f5,f5,f9,f13
	v_f5.f64 = v_f5.f64 >= 0.0 ? v_f9.f64 : v_f13.f64;
	// stfs f5,100(r1)
	temp.f32 = float(v_f5.f64);
	REX_STORE_U32(ctx.r1.u32 + 100, temp.u32);
	// blt cr6,0x82036a50
	if (cr6.lt) goto loc_82036A50;
	// lfs f13,9656(r25)
	temp.u32 = REX_LOAD_U32(r25.u32 + 9656);
	v_f13.f64 = double(temp.f32);
	// lfs f12,9652(r24)
	temp.u32 = REX_LOAD_U32(r24.u32 + 9652);
	v_f12.f64 = double(temp.f32);
loc_82036CD0:
	// lbz r11,15877(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 15877);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82036d48
	if (!cr6.eq) goto loc_82036D48;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fadds f12,f6,f8
	ctx.fpscr.disableFlushMode();
	v_f12.f64 = double(float(v_f6.f64 + v_f8.f64));
	// fadds f11,f5,f7
	v_f11.f64 = double(float(v_f5.f64 + v_f7.f64));
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lfs f0,3416(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3416);
	f0.f64 = double(temp.f32);
	// lis r11,-32232
	r11.s64 = -2112356352;
	// lfs f10,3412(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3412);
	v_f10.f64 = double(temp.f32);
	// lis r10,-32230
	v_r10.s64 = -2112225280;
	// lfs f13,9708(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 9708);
	v_f13.f64 = double(temp.f32);
	// fmuls f12,f12,f23
	v_f12.f64 = double(float(v_f12.f64 * f23.f64));
	// fsubs f9,f30,f13
	v_f9.f64 = double(float(f30.f64 - v_f13.f64));
	// fmuls f11,f11,f23
	v_f11.f64 = double(float(v_f11.f64 * f23.f64));
	// fmadds f13,f9,f10,f13
	v_f13.f64 = double(float(gw_fma(v_f9.f64, v_f10.f64, v_f13.f64)));
	// stfs f13,9708(r11)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(r11.u32 + 9708, temp.u32);
	// lis r11,-32230
	r11.s64 = -2112225280;
	// lfs f13,7552(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 7552);
	v_f13.f64 = double(temp.f32);
	// fsubs f10,f12,f13
	v_f10.f64 = double(float(v_f12.f64 - v_f13.f64));
	// lfs f12,7556(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 7556);
	v_f12.f64 = double(temp.f32);
	// fsubs f11,f11,f12
	v_f11.f64 = double(float(v_f11.f64 - v_f12.f64));
	// fmadds f13,f10,f0,f13
	v_f13.f64 = double(float(gw_fma(v_f10.f64, f0.f64, v_f13.f64)));
	// stfs f13,7552(r10)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(v_r10.u32 + 7552, temp.u32);
	// lfs f13,-11648(r19)
	temp.u32 = REX_LOAD_U32(r19.u32 + -11648);
	v_f13.f64 = double(temp.f32);
	// fmadds f0,f11,f0,f12
	f0.f64 = double(float(gw_fma(v_f11.f64, f0.f64, v_f12.f64)));
	// stfs f0,7556(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + 7556, temp.u32);
	// lis r11,-32230
	r11.s64 = -2112225280;
	// stfs f24,-11316(r11)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(r11.u32 + -11316, temp.u32);
	// b 0x82036fe8
	goto loc_82036FE8;
loc_82036D48:
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsubs f0,f6,f8
	ctx.fpscr.disableFlushMode();
	f0.f64 = double(float(v_f6.f64 - v_f8.f64));
	// fsubs f11,f5,f7
	v_f11.f64 = double(float(v_f5.f64 - v_f7.f64));
	// lis r30,-32230
	r30.s64 = -2112225280;
	// fmuls f2,f13,f31
	v_f2.f64 = double(float(v_f13.f64 * f31.f64));
	// lis r31,-32230
	r31.s64 = -2112225280;
	// fmuls f1,f12,f31
	v_f1.f64 = double(float(v_f12.f64 * f31.f64));
	// lbz r10,32(r26)
	v_r10.u64 = REX_LOAD_U8(r26.u32 + 32);
	// lfs f10,3408(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3408);
	v_f10.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsubs f4,f12,f10
	v_f4.f64 = double(float(v_f12.f64 - v_f10.f64));
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(v_r10.u32, 0, xer);
	// lfs f10,3404(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3404);
	v_f10.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsubs f3,f13,f10
	v_f3.f64 = double(float(v_f13.f64 - v_f10.f64));
	// fadds f10,f6,f8
	v_f10.f64 = double(float(v_f6.f64 + v_f8.f64));
	// fadds f6,f5,f7
	v_f6.f64 = double(float(v_f5.f64 + v_f7.f64));
	// lfs f5,3412(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3412);
	v_f5.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmuls f29,f4,f31
	f29.f64 = double(float(v_f4.f64 * f31.f64));
	// lfs f9,3400(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3400);
	v_f9.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmuls f3,f3,f31
	v_f3.f64 = double(float(v_f3.f64 * f31.f64));
	// lfs f8,3396(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3396);
	v_f8.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmuls f8,f2,f8
	v_f8.f64 = double(float(v_f2.f64 * v_f8.f64));
	// lfs f7,3392(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3392);
	v_f7.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmuls f7,f1,f7
	v_f7.f64 = double(float(v_f1.f64 * v_f7.f64));
	// lfs f4,3388(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3388);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsubs f2,f0,f4
	v_f2.f64 = double(float(f0.f64 - v_f4.f64));
	// lfs f0,3384(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3384);
	f0.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsubs f1,f11,f0
	v_f1.f64 = double(float(v_f11.f64 - f0.f64));
	// lfs f0,1828(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 1828);
	f0.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmuls f4,f10,f0
	v_f4.f64 = double(float(v_f10.f64 * f0.f64));
	// fdivs f11,f2,f29
	v_f11.f64 = double(float(v_f2.f64 / f29.f64));
	// fmuls f6,f6,f0
	v_f6.f64 = double(float(v_f6.f64 * f0.f64));
	// fdivs f10,f1,f3
	v_f10.f64 = double(float(v_f1.f64 / v_f3.f64));
	// fsubs f3,f7,f8
	v_f3.f64 = double(float(v_f7.f64 - v_f8.f64));
	// fsubs f2,f11,f30
	v_f2.f64 = double(float(v_f11.f64 - f30.f64));
	// fmuls f6,f6,f0
	v_f6.f64 = double(float(v_f6.f64 * f0.f64));
	// fsubs f1,f10,f30
	v_f1.f64 = double(float(v_f10.f64 - f30.f64));
	// fsel f8,f3,f7,f8
	v_f8.f64 = v_f3.f64 >= 0.0 ? v_f7.f64 : v_f8.f64;
	// fmuls f7,f4,f0
	v_f7.f64 = double(float(v_f4.f64 * f0.f64));
	// lfd f0,3376(r11)
	f0.u64 = REX_LOAD_U64(r11.u32 + 3376);
	// fneg f4,f10
	v_f4.u64 = v_f10.u64 ^ 0x8000000000000000;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsubs f3,f8,f30
	v_f3.f64 = double(float(v_f8.f64 - f30.f64));
	// fneg f8,f11
	v_f8.u64 = v_f11.u64 ^ 0x8000000000000000;
	// fsel f10,f4,f0,f10
	v_f10.f64 = v_f4.f64 >= 0.0 ? f0.f64 : v_f10.f64;
	// fsel f11,f8,f0,f11
	v_f11.f64 = v_f8.f64 >= 0.0 ? f0.f64 : v_f11.f64;
	// lfd f0,3192(r11)
	f0.u64 = REX_LOAD_U64(r11.u32 + 3192);
	// fsel f10,f1,f0,f10
	v_f10.f64 = v_f1.f64 >= 0.0 ? f0.f64 : v_f10.f64;
	// lfs f8,7556(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 7556);
	v_f8.f64 = double(temp.f32);
	// lis r11,-32232
	r11.s64 = -2112356352;
	// fsel f11,f2,f0,f11
	v_f11.f64 = v_f2.f64 >= 0.0 ? f0.f64 : v_f11.f64;
	// fsubs f2,f6,f8
	v_f2.f64 = double(float(v_f6.f64 - v_f8.f64));
	// frsp f0,f11
	f0.f64 = double(float(v_f11.f64));
	// frsp f11,f10
	v_f11.f64 = double(float(v_f10.f64));
	// lfs f10,7552(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 7552);
	v_f10.f64 = double(temp.f32);
	// fsubs f4,f7,f10
	v_f4.f64 = double(float(v_f7.f64 - v_f10.f64));
	// fmadds f10,f4,f9,f10
	v_f10.f64 = double(float(gw_fma(v_f4.f64, v_f9.f64, v_f10.f64)));
	// stfs f10,7552(r30)
	temp.f32 = float(v_f10.f64);
	REX_STORE_U32(r30.u32 + 7552, temp.u32);
	// fmadds f10,f2,f9,f8
	v_f10.f64 = double(float(gw_fma(v_f2.f64, v_f9.f64, v_f8.f64)));
	// stfs f10,7556(r31)
	temp.f32 = float(v_f10.f64);
	REX_STORE_U32(r31.u32 + 7556, temp.u32);
	// fsubs f10,f0,f11
	v_f10.f64 = double(float(f0.f64 - v_f11.f64));
	// fsel f0,f10,f0,f11
	f0.f64 = v_f10.f64 >= 0.0 ? f0.f64 : v_f11.f64;
	// fmadds f11,f3,f0,f30
	v_f11.f64 = double(float(gw_fma(v_f3.f64, f0.f64, f30.f64)));
	// lfs f0,9708(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 9708);
	f0.f64 = double(temp.f32);
	// fsubs f11,f11,f0
	v_f11.f64 = double(float(v_f11.f64 - f0.f64));
	// fmadds f0,f11,f5,f0
	f0.f64 = double(float(gw_fma(v_f11.f64, v_f5.f64, f0.f64)));
	// stfs f0,9708(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + 9708, temp.u32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f11,3372(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3372);
	v_f11.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmuls f11,f0,f11
	v_f11.f64 = double(float(f0.f64 * v_f11.f64));
	// lfs f10,3368(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3368);
	v_f10.f64 = double(temp.f32);
	// fmuls f0,f0,f10
	f0.f64 = double(float(f0.f64 * v_f10.f64));
	// fsubs f10,f12,f11
	v_f10.f64 = double(float(v_f12.f64 - v_f11.f64));
	// fsubs f9,f13,f0
	v_f9.f64 = double(float(v_f13.f64 - f0.f64));
	// fsel f29,f10,f11,f12
	f29.f64 = v_f10.f64 >= 0.0 ? v_f11.f64 : v_f12.f64;
	// fsel f28,f9,f0,f13
	f28.f64 = v_f9.f64 >= 0.0 ? f0.f64 : v_f13.f64;
	// beq cr6,0x82036f48
	if (cr6.eq) goto loc_82036F48;
	// fadds f0,f7,f31
	f0.f64 = double(float(v_f7.f64 + f31.f64));
	// stfs f0,136(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 136, temp.u32);
	// fadds f0,f6,f31
	f0.f64 = double(float(v_f6.f64 + f31.f64));
	// stfs f0,140(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 140, temp.u32);
	// fsubs f0,f7,f31
	f0.f64 = double(float(v_f7.f64 - f31.f64));
	// stfs f0,144(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 144, temp.u32);
	// addi r5,r1,256
	v_r5.s64 = ctx.r1.s64 + 256;
	// fsubs f0,f6,f31
	f0.f64 = double(float(v_f6.f64 - f31.f64));
	// addi r4,r1,136
	v_r4.s64 = ctx.r1.s64 + 136;
	// stfs f30,256(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 256, temp.u32);
	// addi r3,r1,144
	v_r3.s64 = ctx.r1.s64 + 144;
	// stfs f30,260(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 260, temp.u32);
	// stfs f30,264(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 264, temp.u32);
	// stfs f30,268(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 268, temp.u32);
	// stfs f0,148(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 148, temp.u32);
	// bl 0x8205e068
	ctx.lr = 0x82036EE0;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f6 = v_f6; ctx.f7 = v_f7; ctx.f8 = v_f8; ctx.f9 = v_f9; ctx.f10 = v_f10; ctx.f11 = v_f11; ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8205E068(ctx, base);
	// addi r5,r1,272
	v_r5.s64 = ctx.r1.s64 + 272;
	// stfs f24,272(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 272, temp.u32);
	// addi r4,r1,96
	v_r4.s64 = ctx.r1.s64 + 96;
	// stfs f30,276(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 276, temp.u32);
	// addi r3,r1,88
	v_r3.s64 = ctx.r1.s64 + 88;
	// stfs f24,280(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 280, temp.u32);
	// stfs f30,284(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 284, temp.u32);
	// bl 0x8205e068
	ctx.lr = 0x82036F00;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_8205E068(ctx, base);
	// lfs f0,7552(r30)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r30.u32 + 7552);
	f0.f64 = double(temp.f32);
	// fadds f13,f29,f0
	v_f13.f64 = double(float(f29.f64 + f0.f64));
	// stfs f13,160(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 160, temp.u32);
	// lfs f13,7556(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 7556);
	v_f13.f64 = double(temp.f32);
	// fsubs f0,f0,f29
	f0.f64 = double(float(f0.f64 - f29.f64));
	// stfs f0,176(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 176, temp.u32);
	// addi r5,r1,304
	v_r5.s64 = ctx.r1.s64 + 304;
	// addi r4,r1,160
	v_r4.s64 = ctx.r1.s64 + 160;
	// fadds f12,f28,f13
	v_f12.f64 = double(float(f28.f64 + v_f13.f64));
	// addi r3,r1,176
	v_r3.s64 = ctx.r1.s64 + 176;
	// fsubs f0,f13,f28
	f0.f64 = double(float(v_f13.f64 - f28.f64));
	// stfs f30,304(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 304, temp.u32);
	// stfs f24,308(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 308, temp.u32);
	// stfs f24,312(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 312, temp.u32);
	// stfs f30,316(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 316, temp.u32);
	// stfs f12,164(r1)
	temp.f32 = float(v_f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 164, temp.u32);
	// stfs f0,180(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 180, temp.u32);
	// bl 0x8205e068
	ctx.lr = 0x82036F48;
	ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_8205E068(ctx, base);
	v_f1 = ctx.f1; v_f2 = ctx.f2; v_f3 = ctx.f3; v_f4 = ctx.f4; v_f5 = ctx.f5; v_f6 = ctx.f6; v_f7 = ctx.f7; v_f8 = ctx.f8; v_f9 = ctx.f9; v_f10 = ctx.f10; v_f11 = ctx.f11; v_f12 = ctx.f12; v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_82036F48:
	// lis r11,-32230
	r11.s64 = -2112225280;
	// lfs f13,-11648(r19)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r19.u32 + -11648);
	v_f13.f64 = double(temp.f32);
	// stfs f24,-11316(r11)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(r11.u32 + -11316, temp.u32);
	// b 0x82036fe8
	goto loc_82036FE8;
loc_82036F58:
	// lis r11,-32230
	r11.s64 = -2112225280;
	// lfs f13,-11648(r19)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r19.u32 + -11648);
	v_f13.f64 = double(temp.f32);
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lfs f0,-11316(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + -11316);
	f0.f64 = double(temp.f32);
	// fadds f0,f0,f13
	f0.f64 = double(float(f0.f64 + v_f13.f64));
	// stfs f0,-11316(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + -11316, temp.u32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f8,3412(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3412);
	v_f8.f64 = double(temp.f32);
	// lis r10,-32230
	v_r10.s64 = -2112225280;
	// lfs f10,3416(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3416);
	v_f10.f64 = double(temp.f32);
	// lis r11,-32232
	r11.s64 = -2112356352;
	// fneg f12,f0
	v_f12.u64 = f0.u64 ^ 0x8000000000000000;
	// lfs f9,9708(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 9708);
	v_f9.f64 = double(temp.f32);
	// fsubs f11,f0,f30
	v_f11.f64 = double(float(f0.f64 - f30.f64));
	// fsubs f7,f30,f9
	v_f7.f64 = double(float(f30.f64 - v_f9.f64));
	// fmadds f9,f7,f8,f9
	v_f9.f64 = double(float(gw_fma(v_f7.f64, v_f8.f64, v_f9.f64)));
	// stfs f9,9708(r11)
	temp.f32 = float(v_f9.f64);
	REX_STORE_U32(r11.u32 + 9708, temp.u32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfd f9,3376(r11)
	v_f9.u64 = REX_LOAD_U64(r11.u32 + 3376);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsel f8,f12,f9,f0
	v_f8.f64 = v_f12.f64 >= 0.0 ? v_f9.f64 : f0.f64;
	// fsel f12,f12,f9,f0
	v_f12.f64 = v_f12.f64 >= 0.0 ? v_f9.f64 : f0.f64;
	// lfd f0,3192(r11)
	f0.u64 = REX_LOAD_U64(r11.u32 + 3192);
	// lis r11,-32230
	r11.s64 = -2112225280;
	// fsel f9,f11,f0,f8
	v_f9.f64 = v_f11.f64 >= 0.0 ? f0.f64 : v_f8.f64;
	// fsel f0,f11,f0,f12
	f0.f64 = v_f11.f64 >= 0.0 ? f0.f64 : v_f12.f64;
	// frsp f12,f9
	v_f12.f64 = double(float(v_f9.f64));
	// frsp f11,f0
	v_f11.f64 = double(float(f0.f64));
	// lfs f0,7552(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 7552);
	f0.f64 = double(temp.f32);
	// fmuls f9,f12,f0
	v_f9.f64 = double(float(v_f12.f64 * f0.f64));
	// lfs f12,7556(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 7556);
	v_f12.f64 = double(temp.f32);
	// fmuls f11,f11,f12
	v_f11.f64 = double(float(v_f11.f64 * v_f12.f64));
	// fnmsubs f0,f9,f10,f0
	f0.f64 = double(float(-gw_fma(v_f9.f64, v_f10.f64, -f0.f64)));
	// stfs f0,7552(r10)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(v_r10.u32 + 7552, temp.u32);
	// fnmsubs f0,f11,f10,f12
	f0.f64 = double(float(-gw_fma(v_f11.f64, v_f10.f64, -v_f12.f64)));
	// stfs f0,7556(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + 7556, temp.u32);
loc_82036FE8:
	// lis r11,-32230
	r11.s64 = -2112225280;
	// addi r11,r11,-11380
	r11.s64 = r11.s64 + -11380;
	// lbz r11,32(r11)
	r11.u64 = REX_LOAD_U8(r11.u32 + 32);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037144
	if (cr6.eq) goto loc_82037144;
	// lis r11,-32232
	r11.s64 = -2112356352;
	// fmr f31,f24
	ctx.fpscr.disableFlushMode();
	f31.f64 = f24.f64;
	// lis r31,-32230
	r31.s64 = -2112225280;
	// lis r10,-32230
	v_r10.s64 = -2112225280;
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// lfs f0,5288(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 5288);
	f0.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmuls f0,f0,f13
	f0.f64 = double(float(f0.f64 * v_f13.f64));
	// lfs f12,-11320(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -11320);
	v_f12.f64 = double(temp.f32);
	// lfs f13,3364(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3364);
	v_f13.f64 = double(temp.f32);
	// lis r11,-32232
	r11.s64 = -2112356352;
	// addi r11,r11,5080
	r11.s64 = r11.s64 + 5080;
	// fadds f11,f0,f12
	v_f11.f64 = double(float(f0.f64 + v_f12.f64));
	// lfs f12,-11324(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + -11324);
	v_f12.f64 = double(temp.f32);
	// fmadds f12,f0,f13,f12
	v_f12.f64 = double(float(gw_fma(f0.f64, v_f13.f64, v_f12.f64)));
	// lfd f0,3328(r8)
	f0.u64 = REX_LOAD_U64(v_r8.u32 + 3328);
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// lwz r9,4(r11)
	v_r9.u64 = REX_LOAD_U32(r11.u32 + 4);
	// lwz r11,0(r9)
	r11.u64 = REX_LOAD_U32(v_r9.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// fmul f13,f11,f0
	v_f13.f64 = v_f11.f64 * f0.f64;
	// fmul f0,f12,f0
	f0.f64 = v_f12.f64 * f0.f64;
	// fctid f12,f13
	v_f12.s64 = __builtin_isnan(v_f13.f64) ? int64_t(0x8000000000000000ULL) : (v_f13.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvtsd_si64(simde_mm_load_sd(&v_f13.f64));
	// fctid f11,f0
	v_f11.s64 = __builtin_isnan(f0.f64) ? int64_t(0x8000000000000000ULL) : (f0.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvtsd_si64(simde_mm_load_sd(&f0.f64));
	// fcfid f12,f12
	v_f12.f64 = double(v_f12.s64);
	// fcfid f11,f11
	v_f11.f64 = double(v_f11.s64);
	// fsub f13,f13,f12
	v_f13.f64 = v_f13.f64 - v_f12.f64;
	// fsub f12,f0,f11
	v_f12.f64 = f0.f64 - v_f11.f64;
	// lfd f0,3320(r8)
	f0.u64 = REX_LOAD_U64(v_r8.u32 + 3320);
	// fmul f13,f13,f0
	v_f13.f64 = v_f13.f64 * f0.f64;
	// fmul f12,f12,f0
	v_f12.f64 = v_f12.f64 * f0.f64;
	// frsp f0,f13
	f0.f64 = double(float(v_f13.f64));
	// stfs f0,-11320(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + -11320, temp.u32);
	// frsp f1,f12
	v_f1.f64 = double(float(v_f12.f64));
	// stfs f1,-11324(r10)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(v_r10.u32 + -11324, temp.u32);
	// beq cr6,0x820370ec
	if (cr6.eq) goto loc_820370EC;
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lfs f13,3360(r8)
	temp.u32 = REX_LOAD_U32(v_r8.u32 + 3360);
	v_f13.f64 = double(temp.f32);
	// lfs f12,1780(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 1780);
	v_f12.f64 = double(temp.f32);
loc_8203709C:
	
	// lwz r10,104(r11)
	v_r10.u64 = REX_LOAD_U32(r11.u32 + 104);
	// cmpwi cr6,r10,0
	cr6.compare<int32_t>(v_r10.s32, 0, xer);
	// bne cr6,0x820370dc
	if (!cr6.eq) goto loc_820370DC;
	// lfs f11,108(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 108);
	v_f11.f64 = double(temp.f32);
	// lfs f0,116(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 116);
	f0.f64 = double(temp.f32);
	// fdivs f0,f0,f11
	f0.f64 = double(float(f0.f64 / v_f11.f64));
	// fcmpu cr6,f0,f23
	cr6.compare(f0.f64, f23.f64);
	// lfs f0,116(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 116);
	f0.f64 = double(temp.f32);
	// fdivs f0,f0,f11
	f0.f64 = double(float(f0.f64 / v_f11.f64));
	// bge cr6,0x820370cc
	if (!cr6.lt) goto loc_820370CC;
	// fmuls f0,f0,f12
	f0.f64 = double(float(f0.f64 * v_f12.f64));
	// b 0x820370d4
	goto loc_820370D4;
loc_820370CC:
	// fsubs f0,f0,f23
	ctx.fpscr.disableFlushMode();
	f0.f64 = double(float(f0.f64 - f23.f64));
	// fnmsubs f0,f0,f13,f30
	f0.f64 = double(float(-gw_fma(f0.f64, v_f13.f64, -f30.f64)));
loc_820370D4:
	// fsubs f11,f31,f0
	ctx.fpscr.disableFlushMode();
	v_f11.f64 = double(float(f31.f64 - f0.f64));
	// fsel f31,f11,f31,f0
	f31.f64 = v_f11.f64 >= 0.0 ? f31.f64 : f0.f64;
loc_820370DC:
	// lwz r9,4(r9)
	v_r9.u64 = REX_LOAD_U32(v_r9.u32 + 4);
	// lwz r11,0(r9)
	r11.u64 = REX_LOAD_U32(v_r9.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x8203709c
	if (!cr6.eq) goto loc_8203709C;
loc_820370EC:
	// bl 0x8212ea80
	ctx.lr = 0x820370F0;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f6 = v_f6; ctx.f7 = v_f7; ctx.f8 = v_f8; ctx.f9 = v_f9; ctx.f10 = v_f10; ctx.f11 = v_f11; ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8212EA80(ctx, base);
	v_f1 = ctx.f1;
	// fmr f0,f1
	ctx.fpscr.disableFlushMode();
	f0.f64 = v_f1.f64;
	// fmuls f13,f31,f31
	v_f13.f64 = double(float(f31.f64 * f31.f64));
	// lfs f1,-11320(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -11320);
	v_f1.f64 = double(temp.f32);
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// fmuls f31,f0,f13
	f31.f64 = double(float(f0.f64 * v_f13.f64));
	// bl 0x8212ea80
	ctx.lr = 0x82037108;
	ctx.f1 = v_f1; ctx.f13 = v_f13;
	sub_8212EA80(ctx, base);
	v_f1 = ctx.f1;
	// fmr f0,f1
	ctx.fpscr.disableFlushMode();
	f0.f64 = v_f1.f64;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f1,-11320(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + -11320);
	v_f1.f64 = double(temp.f32);
	// lfs f29,1804(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 1804);
	f29.f64 = double(temp.f32);
	// lis r11,-32230
	r11.s64 = -2112225280;
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// fmuls f0,f0,f31
	f0.f64 = double(float(f0.f64 * f31.f64));
	// fmuls f0,f0,f29
	f0.f64 = double(float(f0.f64 * f29.f64));
	// stfs f0,7560(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + 7560, temp.u32);
	// bl 0x8212e9b0
	ctx.lr = 0x82037130;
	ctx.f1 = v_f1;
	sub_8212E9B0(ctx, base);
	v_f1 = ctx.f1; v_f2 = ctx.f2; v_f3 = ctx.f3; v_f4 = ctx.f4; v_f5 = ctx.f5; v_f6 = ctx.f6; v_f7 = ctx.f7; v_f8 = ctx.f8; v_f9 = ctx.f9; v_f10 = ctx.f10; v_f11 = ctx.f11; v_f12 = ctx.f12; v_f13 = ctx.f13; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// frsp f0,f1
	ctx.fpscr.disableFlushMode();
	f0.f64 = double(float(v_f1.f64));
	// lis r11,-32230
	r11.s64 = -2112225280;
	// fmuls f0,f0,f31
	f0.f64 = double(float(f0.f64 * f31.f64));
	// fmuls f0,f0,f29
	f0.f64 = double(float(f0.f64 * f29.f64));
	// stfs f0,7564(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + 7564, temp.u32);
loc_82037144:
	// bl 0x82033610
	ctx.lr = 0x82037148;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f6 = v_f6; ctx.f7 = v_f7; ctx.f8 = v_f8; ctx.f9 = v_f9; ctx.f10 = v_f10; ctx.f11 = v_f11; ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82033610(ctx, base);
	v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037398
	if (!cr6.eq) goto loc_82037398;
	// lis r11,-32225
	r11.s64 = -2111897600;
	// lbz r11,-18731(r11)
	r11.u64 = REX_LOAD_U8(r11.u32 + -18731);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037398
	if (!cr6.eq) goto loc_82037398;
	// lis r11,-32230
	r11.s64 = -2112225280;
	// addi r11,r11,-11556
	r11.s64 = r11.s64 + -11556;
	// lbz r11,32(r11)
	r11.u64 = REX_LOAD_U8(r11.u32 + 32);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037398
	if (cr6.eq) goto loc_82037398;
	// lis r10,-32230
	v_r10.s64 = -2112225280;
	// lwz r11,-11328(r10)
	r11.u64 = REX_LOAD_U32(v_r10.u32 + -11328);
	// clrlwi r9,r11,31
	v_r9.u64 = r11.u32 & 0x1;
	// cmplwi cr6,r9,0
	cr6.compare<uint32_t>(v_r9.u32, 0, xer);
	// lis r9,-32230
	v_r9.s64 = -2112225280;
	// addi r30,r9,-11344
	r30.s64 = v_r9.s64 + -11344;
	// bne cr6,0x820371ac
	if (!cr6.eq) goto loc_820371AC;
	// ori r11,r11,1
	r11.u64 = r11.u64 | 1;
	// stfs f30,0(r30)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f30.f64);
	REX_STORE_U32(r30.u32 + 0, temp.u32);
	// stfs f30,4(r30)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(r30.u32 + 4, temp.u32);
	// stfs f30,8(r30)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(r30.u32 + 8, temp.u32);
	// stfs f30,12(r30)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(r30.u32 + 12, temp.u32);
	// stw r11,-11328(r10)
	REX_STORE_U32(v_r10.u32 + -11328, r11.u32);
loc_820371AC:
	// lis r29,-32230
	r29.s64 = -2112225280;
	// lwz r11,-11636(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + -11636);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x8203726c
	if (!cr6.eq) goto loc_8203726C;
	// lis r11,-32230
	r11.s64 = -2112225280;
	// addi r11,r11,-11592
	r11.s64 = r11.s64 + -11592;
	// lbz r11,32(r11)
	r11.u64 = REX_LOAD_U8(r11.u32 + 32);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x8203726c
	if (cr6.eq) goto loc_8203726C;
	// li r3,172
	v_r3.s64 = 172;
	// bl 0x820e8130
	ctx.lr = 0x820371D8;
	ctx.r3 = v_r3; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820E8130(ctx, base);
	v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8;
	// mr r31,r3
	r31.u64 = v_r3.u64;
	// cmplwi cr6,r31,0
	cr6.compare<uint32_t>(r31.u32, 0, xer);
	// beq cr6,0x8203726c
	if (cr6.eq) goto loc_8203726C;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f0,9656(r25)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r25.u32 + 9656);
	f0.f64 = double(temp.f32);
	// mr r3,r23
	v_r3.u64 = r23.u64;
	// lfs f31,3356(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3356);
	f31.f64 = double(temp.f32);
	// fmuls f28,f0,f31
	f28.f64 = double(float(f0.f64 * f31.f64));
	// bl 0x82030058
	ctx.lr = 0x820371FC;
	ctx.r3 = v_r3;
	sub_82030058(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f0,9652(r24)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r24.u32 + 9652);
	f0.f64 = double(temp.f32);
	// fmuls f31,f0,f31
	f31.f64 = double(float(f0.f64 * f31.f64));
	// lfs f29,1736(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 1736);
	f29.f64 = double(temp.f32);
	// clrldi r11,r3,32
	r11.u64 = v_r3.u64 & 0xFFFFFFFF;
	// mr r3,r23
	v_r3.u64 = r23.u64;
	// std r11,112(r1)
	REX_STORE_U64(ctx.r1.u32 + 112, r11.u64);
	// lfd f0,112(r1)
	f0.u64 = REX_LOAD_U64(ctx.r1.u32 + 112);
	// fcfid f0,f0
	f0.f64 = double(f0.s64);
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// fmsubs f0,f0,f29,f30
	f0.f64 = double(float(gw_fma(f0.f64, f29.f64, -f30.f64)));
	// fmuls f28,f0,f28
	f28.f64 = double(float(f0.f64 * f28.f64));
	// bl 0x82030058
	ctx.lr = 0x82037230;
	ctx.r3 = v_r3;
	sub_82030058(ctx, base);
	v_r3 = ctx.r3;
	// clrldi r11,r3,32
	r11.u64 = v_r3.u64 & 0xFFFFFFFF;
	// stfs f28,132(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f28.f64);
	REX_STORE_U32(ctx.r1.u32 + 132, temp.u32);
	// addi r4,r1,128
	v_r4.s64 = ctx.r1.s64 + 128;
	// mr r3,r31
	v_r3.u64 = r31.u64;
	// std r11,112(r1)
	REX_STORE_U64(ctx.r1.u32 + 112, r11.u64);
	// lfd f0,112(r1)
	f0.u64 = REX_LOAD_U64(ctx.r1.u32 + 112);
	// fcfid f0,f0
	f0.f64 = double(f0.s64);
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// fmsubs f0,f0,f29,f30
	f0.f64 = double(float(gw_fma(f0.f64, f29.f64, -f30.f64)));
	// fmuls f0,f0,f31
	f0.f64 = double(float(f0.f64 * f31.f64));
	// stfs f0,128(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 128, temp.u32);
	// bl 0x820499e0
	ctx.lr = 0x82037260;
	ctx.r3 = v_r3; ctx.r4 = v_r4;
	sub_820499E0(ctx, base);
	v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8;
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(v_r3.u32, 0, xer);
	// beq cr6,0x8203726c
	if (cr6.eq) goto loc_8203726C;
	// bl 0x82048638
	ctx.lr = 0x8203726C;
	sub_82048638(ctx, base);
	v_r7 = ctx.r7; v_r8 = ctx.r8;
loc_8203726C:
	// lis r10,6990
	v_r10.s64 = 458096640;
	// lwz r11,-11636(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + -11636);
	// ori r31,r10,33205
	r31.u64 = v_r10.u64 | 33205;
	// mulhw r10,r11,r31
	v_r10.s64 = (int64_t(r11.s32) * int64_t(r31.s32)) >> 32;
	// srawi r10,r10,4
	xer.ca = (v_r10.s32 < 0) & ((v_r10.u32 & 0xF) != 0);
	v_r10.s64 = v_r10.s32 >> 4;
	// rlwinm r9,r10,1,31,31
	v_r9.u64 = __builtin_rotateleft64(v_r10.u32 | (v_r10.u64 << 32), 1) & 0x1;
	// add r10,r10,r9
	v_r10.u64 = v_r10.u64 + v_r9.u64;
	// mulli r10,r10,150
	v_r10.s64 = static_cast<int64_t>(v_r10.u64 * static_cast<uint64_t>(150));
	// subf. r11,r10,r11
	r11.u64 = r11.u64 - v_r10.u64;
	cr0.compare<int32_t>(r11.s32, 0, xer);
	// bne 0x820372c0
	if (!cr0.eq) goto loc_820372C0;
	// addi r3,r1,432
	v_r3.s64 = ctx.r1.s64 + 432;
	// fmr f1,f30
	ctx.fpscr.disableFlushMode();
	v_f1.f64 = f30.f64;
	// bl 0x820304c8
	ctx.lr = 0x820372A0;
	ctx.f1 = v_f1; ctx.r3 = v_r3; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820304C8(ctx, base);
	v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8;
	// lfs f0,0(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r3.u32 + 0);
	f0.f64 = double(temp.f32);
	// stfs f0,0(r30)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r30.u32 + 0, temp.u32);
	// lfs f0,4(r3)
	temp.u32 = REX_LOAD_U32(v_r3.u32 + 4);
	f0.f64 = double(temp.f32);
	// stfs f0,4(r30)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r30.u32 + 4, temp.u32);
	// lfs f0,8(r3)
	temp.u32 = REX_LOAD_U32(v_r3.u32 + 8);
	f0.f64 = double(temp.f32);
	// stfs f0,8(r30)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r30.u32 + 8, temp.u32);
	// lfs f0,12(r3)
	temp.u32 = REX_LOAD_U32(v_r3.u32 + 12);
	f0.f64 = double(temp.f32);
	// stfs f0,12(r30)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r30.u32 + 12, temp.u32);
loc_820372C0:
	// lwz r11,-11636(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + -11636);
	// srawi r10,r11,1
	xer.ca = (r11.s32 < 0) & ((r11.u32 & 0x1) != 0);
	v_r10.s64 = r11.s32 >> 1;
	// addze r10,r10
	temp.s64 = v_r10.s64 + xer.ca;
	xer.ca = temp.u32 < v_r10.u32;
	v_r10.s64 = temp.s64;
	// rlwinm r10,r10,1,0,30
	v_r10.u64 = __builtin_rotateleft64(v_r10.u32 | (v_r10.u64 << 32), 1) & 0xFFFFFFFE;
	// subf. r11,r10,r11
	r11.u64 = r11.u64 - v_r10.u64;
	cr0.compare<int32_t>(r11.s32, 0, xer);
	// bne 0x82037374
	if (!cr0.eq) goto loc_82037374;
	// lis r6,-32256
	v_r6.s64 = -2113929216;
	// lis r7,-32256
	v_r7.s64 = -2113929216;
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f25,3352(r6)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r6.u32 + 3352);
	f25.f64 = double(temp.f32);
	// lfs f26,3348(r7)
	temp.u32 = REX_LOAD_U32(v_r7.u32 + 3348);
	f26.f64 = double(temp.f32);
	// li r30,10
	r30.s64 = 10;
	// lfs f27,1816(r8)
	temp.u32 = REX_LOAD_U32(v_r8.u32 + 1816);
	f27.f64 = double(temp.f32);
	// lfs f28,3344(r9)
	temp.u32 = REX_LOAD_U32(v_r9.u32 + 3344);
	f28.f64 = double(temp.f32);
	// lfs f29,3340(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3340);
	f29.f64 = double(temp.f32);
	// lfs f31,3336(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3336);
	f31.f64 = double(temp.f32);
loc_8203730C:
	
	// addi r3,r1,112
	v_r3.s64 = ctx.r1.s64 + 112;
	// fmr f1,f24
	ctx.fpscr.disableFlushMode();
	v_f1.f64 = f24.f64;
	// bl 0x8203bc08
	ctx.lr = 0x82037318;
	ctx.f1 = v_f1; ctx.r3 = v_r3; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8203BC08(ctx, base);
	// lfs f0,112(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 112);
	f0.f64 = double(temp.f32);
	// stfs f0,400(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 400, temp.u32);
	// addi r7,r1,336
	v_r7.s64 = ctx.r1.s64 + 336;
	// addi r6,r1,368
	v_r6.s64 = ctx.r1.s64 + 368;
	// lfs f0,116(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 116);
	f0.f64 = double(temp.f32);
	// li r5,20
	v_r5.s64 = 20;
	// stfs f31,336(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 336, temp.u32);
	// addi r3,r1,400
	v_r3.s64 = ctx.r1.s64 + 400;
	// stfs f29,340(r1)
	temp.f32 = float(f29.f64);
	REX_STORE_U32(ctx.r1.u32 + 340, temp.u32);
	// stfs f28,344(r1)
	temp.f32 = float(f28.f64);
	REX_STORE_U32(ctx.r1.u32 + 344, temp.u32);
	// fmr f1,f25
	v_f1.f64 = f25.f64;
	// stfs f30,348(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 348, temp.u32);
	// stfs f23,368(r1)
	temp.f32 = float(f23.f64);
	REX_STORE_U32(ctx.r1.u32 + 368, temp.u32);
	// stfs f27,372(r1)
	temp.f32 = float(f27.f64);
	REX_STORE_U32(ctx.r1.u32 + 372, temp.u32);
	// stfs f26,376(r1)
	temp.f32 = float(f26.f64);
	REX_STORE_U32(ctx.r1.u32 + 376, temp.u32);
	// stfs f30,380(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 380, temp.u32);
	// stfs f0,404(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 404, temp.u32);
	// stfs f24,408(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 408, temp.u32);
	// stfs f24,412(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 412, temp.u32);
	// bl 0x82030c58
	ctx.lr = 0x82037368;
	ctx.f1 = v_f1; ctx.r3 = v_r3; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7;
	sub_82030C58(ctx, base);
	v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// addi r30,r30,-1
	r30.s64 = r30.s64 + -1;
	// cmplwi cr6,r30,0
	cr6.compare<uint32_t>(r30.u32, 0, xer);
	// bne cr6,0x8203730c
	if (!cr6.eq) goto loc_8203730C;
loc_82037374:
	// lwz r11,-11636(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + -11636);
	// addi r11,r11,1
	r11.s64 = r11.s64 + 1;
	// mulhw r10,r11,r31
	v_r10.s64 = (int64_t(r11.s32) * int64_t(r31.s32)) >> 32;
	// srawi r10,r10,5
	xer.ca = (v_r10.s32 < 0) & ((v_r10.u32 & 0x1F) != 0);
	v_r10.s64 = v_r10.s32 >> 5;
	// rlwinm r9,r10,1,31,31
	v_r9.u64 = __builtin_rotateleft64(v_r10.u32 | (v_r10.u64 << 32), 1) & 0x1;
	// add r10,r10,r9
	v_r10.u64 = v_r10.u64 + v_r9.u64;
	// mulli r10,r10,300
	v_r10.s64 = static_cast<int64_t>(v_r10.u64 * static_cast<uint64_t>(300));
	// subf r11,r10,r11
	r11.u64 = r11.u64 - v_r10.u64;
	// stw r11,-11636(r29)
	REX_STORE_U32(r29.u32 + -11636, r11.u32);
loc_82037398:
	// lbz r11,14861(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 14861);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x820373c8
	if (cr6.eq) goto loc_820373C8;
	// bl 0x8204f3b0
	ctx.lr = 0x820373A8;
	ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8204F3B0(ctx, base);
	v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x820373c8
	if (!cr6.eq) goto loc_820373C8;
	// li r3,80
	v_r3.s64 = 80;
	// bl 0x820787f8
	ctx.lr = 0x820373BC;
	ctx.r3 = v_r3;
	sub_820787F8(ctx, base);
	// bl 0x82036358
	ctx.lr = 0x820373C0;
	sub_82036358(ctx, base);
	v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// mr r11,r20
	r11.u64 = r20.u64;
	// stb r11,14861(r22)
	REX_STORE_U8(r22.u32 + 14861, r11.u8);
loc_820373C8:
	// lbz r11,15877(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 15877);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x820374dc
	if (cr6.eq) goto loc_820374DC;
	// lwz r6,15348(r22)
	v_r6.u64 = REX_LOAD_U32(r22.u32 + 15348);
	// cmpwi cr6,r6,1
	cr6.compare<int32_t>(v_r6.s32, 1, xer);
	// ble cr6,0x820374d8
	if (!cr6.gt) goto loc_820374D8;
	// ld r9,15392(r22)
	v_r9.u64 = REX_LOAD_U64(r22.u32 + 15392);
	// mr r7,r20
	v_r7.u64 = r20.u64;
	// li r10,1
	v_r10.s64 = 1;
	// addi r8,r22,15512
	v_r8.s64 = r22.s64 + 15512;
loc_820373F0:
	
	// ld r11,0(r8)
	r11.u64 = REX_LOAD_U64(v_r8.u32 + 0);
	// cmpd cr6,r11,r9
	cr6.compare<int64_t>(r11.s64, v_r9.s64, xer);
	// ble cr6,0x82037410
	if (!cr6.gt) goto loc_82037410;
	// cmpwi cr6,r7,0
	cr6.compare<int32_t>(v_r7.s32, 0, xer);
	// blt cr6,0x82037418
	if (cr6.lt) goto loc_82037418;
	// mr r7,r10
	v_r7.u64 = v_r10.u64;
	// mr r9,r11
	v_r9.u64 = r11.u64;
	// b 0x82037418
	goto loc_82037418;
loc_82037410:
	// bne cr6,0x82037418
	if (!cr6.eq) goto loc_82037418;
	// li r7,-1
	v_r7.s64 = -1;
loc_82037418:
	// addi r10,r10,1
	v_r10.s64 = v_r10.s64 + 1;
	// addi r8,r8,120
	v_r8.s64 = v_r8.s64 + 120;
	// cmpw cr6,r10,r6
	cr6.compare<int32_t>(v_r10.s32, v_r6.s32, xer);
	// blt cr6,0x820373f0
	if (cr6.lt) goto loc_820373F0;
	// lwz r11,14856(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14856);
	// cmpw cr6,r7,r11
	cr6.compare<int32_t>(v_r7.s32, r11.s32, xer);
	// beq cr6,0x820374d8
	if (cr6.eq) goto loc_820374D8;
	// stw r7,14856(r22)
	REX_STORE_U32(r22.u32 + 14856, v_r7.u32);
	// li r9,-1
	v_r9.s64 = -1;
	// lwz r8,14912(r22)
	v_r8.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// mr r11,r20
	r11.u64 = r20.u64;
	// cmpwi cr6,r8,0
	cr6.compare<int32_t>(v_r8.s32, 0, xer);
	// ble cr6,0x820374d8
	if (!cr6.gt) goto loc_820374D8;
	// addi r10,r22,15020
	v_r10.s64 = r22.s64 + 15020;
loc_82037450:
	
	// lwz r6,-8(r10)
	v_r6.u64 = REX_LOAD_U32(v_r10.u32 + -8);
	// cmpw cr6,r6,r7
	cr6.compare<int32_t>(v_r6.s32, v_r7.s32, xer);
	// bne cr6,0x82037478
	if (!cr6.eq) goto loc_82037478;
	// cmpwi cr6,r9,0
	cr6.compare<int32_t>(v_r9.s32, 0, xer);
	// bge cr6,0x8203746c
	if (!cr6.lt) goto loc_8203746C;
	// lwz r9,0(r10)
	v_r9.u64 = REX_LOAD_U32(v_r10.u32 + 0);
	// b 0x82037478
	goto loc_82037478;
loc_8203746C:
	// cmpw cr6,r11,r9
	cr6.compare<int32_t>(r11.s32, v_r9.s32, xer);
	// bgt cr6,0x82037478
	if (cr6.gt) goto loc_82037478;
	// mr r9,r11
	v_r9.u64 = r11.u64;
loc_82037478:
	// addi r11,r11,1
	r11.s64 = r11.s64 + 1;
	// addi r10,r10,108
	v_r10.s64 = v_r10.s64 + 108;
	// cmpw cr6,r11,r8
	cr6.compare<int32_t>(r11.s32, v_r8.s32, xer);
	// blt cr6,0x82037450
	if (cr6.lt) goto loc_82037450;
	// cmplwi cr6,r9,3
	cr6.compare<uint32_t>(v_r9.u32, 3, xer);
	// bgt cr6,0x820374d8
	if (cr6.gt) goto loc_820374D8;
	// lis r12,-32253
	r12.s64 = -2113732608;
	// addi r12,r12,29864
	r12.s64 = r12.s64 + 29864;
	// rlwinm r0,r9,2,0,29
	r0.u64 = __builtin_rotateleft64(v_r9.u32 | (v_r9.u64 << 32), 2) & 0xFFFFFFFC;
	// lwzx r0,r12,r0
	r0.u64 = REX_LOAD_U32(r12.u32 + r0.u32);
	// mtctr r0
	ctr.u64 = r0.u64;
	// bctr 
	switch (v_r9.u32) {
	case 0:
		goto loc_820374B8;
	case 1:
		goto loc_820374C0;
	case 2:
		goto loc_820374C8;
	case 3:
		goto loc_820374D0;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_820374B8:
	// li r3,82
	v_r3.s64 = 82;
	// b 0x820374d4
	goto loc_820374D4;
loc_820374C0:
	// li r3,83
	v_r3.s64 = 83;
	// b 0x820374d4
	goto loc_820374D4;
loc_820374C8:
	// li r3,84
	v_r3.s64 = 84;
	// b 0x820374d4
	goto loc_820374D4;
loc_820374D0:
	// li r3,85
	v_r3.s64 = 85;
loc_820374D4:
	// bl 0x820787f8
	ctx.lr = 0x820374D8;
	ctx.r3 = v_r3; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820787F8(ctx, base);
	v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_820374D8:
	// bl 0x8204ebe8
	ctx.lr = 0x820374DC;
	ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8204EBE8(ctx, base);
	v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_820374DC:
	// bl 0x82033610
	ctx.lr = 0x820374E0;
	ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82033610(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037abc
	if (cr6.eq) goto loc_82037ABC;
	// bl 0x82033638
	ctx.lr = 0x820374F0;
	sub_82033638(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037abc
	if (!cr6.eq) goto loc_82037ABC;
	// mr r29,r20
	r29.u64 = r20.u64;
	// mr r28,r20
	r28.u64 = r20.u64;
	// bl 0x8204f3b0
	ctx.lr = 0x82037508;
	sub_8204F3B0(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x820375b4
	if (!cr6.eq) goto loc_820375B4;
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// li r9,1
	v_r9.s64 = 1;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x8203757c
	if (!cr6.gt) goto loc_8203757C;
	// rotlwi r10,r11,0
	v_r10.u64 = __builtin_rotateleft32(r11.u32, 0);
	// addi r11,r22,15012
	r11.s64 = r22.s64 + 15012;
loc_8203752C:
	
	// lwz r8,15836(r22)
	v_r8.u64 = REX_LOAD_U32(r22.u32 + 15836);
	// cmpwi cr6,r8,0
	cr6.compare<int32_t>(v_r8.s32, 0, xer);
	// bne cr6,0x8203755c
	if (!cr6.eq) goto loc_8203755C;
	// lbz r8,-52(r11)
	v_r8.u64 = REX_LOAD_U8(r11.u32 + -52);
	// cmplwi cr6,r8,0
	cr6.compare<uint32_t>(v_r8.u32, 0, xer);
	// bne cr6,0x8203755c
	if (!cr6.eq) goto loc_8203755C;
	// lwz r8,0(r11)
	v_r8.u64 = REX_LOAD_U32(r11.u32 + 0);
	// addi r7,r22,15400
	v_r7.s64 = r22.s64 + 15400;
	// mulli r8,r8,120
	v_r8.s64 = static_cast<int64_t>(v_r8.u64 * static_cast<uint64_t>(120));
	// lwzx r8,r8,r7
	v_r8.u64 = REX_LOAD_U32(v_r8.u32 + v_r7.u32);
	// cmpwi cr6,r8,0
	cr6.compare<int32_t>(v_r8.s32, 0, xer);
	// ble cr6,0x8203756c
	if (!cr6.gt) goto loc_8203756C;
loc_8203755C:
	// lfs f0,-92(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + -92);
	f0.f64 = double(temp.f32);
	// fcmpu cr6,f0,f30
	cr6.compare(f0.f64, f30.f64);
	// bge cr6,0x8203756c
	if (!cr6.lt) goto loc_8203756C;
	// mr r9,r20
	v_r9.u64 = r20.u64;
loc_8203756C:
	// addi r10,r10,-1
	v_r10.s64 = v_r10.s64 + -1;
	// addi r11,r11,108
	r11.s64 = r11.s64 + 108;
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(v_r10.u32, 0, xer);
	// bne cr6,0x8203752c
	if (!cr6.eq) goto loc_8203752C;
loc_8203757C:
	// clrlwi r11,r9,24
	r11.u64 = v_r9.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x820375b4
	if (cr6.eq) goto loc_820375B4;
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// mr r10,r20
	v_r10.u64 = r20.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x820375b4
	if (!cr6.gt) goto loc_820375B4;
	// addi r11,r22,14920
	r11.s64 = r22.s64 + 14920;
loc_8203759C:
	
	// stfs f30,0(r11)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f30.f64);
	REX_STORE_U32(r11.u32 + 0, temp.u32);
	// lwz r9,14912(r22)
	v_r9.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// addi r10,r10,1
	v_r10.s64 = v_r10.s64 + 1;
	// addi r11,r11,108
	r11.s64 = r11.s64 + 108;
	// cmpw cr6,r10,r9
	cr6.compare<int32_t>(v_r10.s32, v_r9.s32, xer);
	// blt cr6,0x8203759c
	if (cr6.lt) goto loc_8203759C;
loc_820375B4:
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// mr r30,r20
	r30.u64 = r20.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x82037728
	if (!cr6.gt) goto loc_82037728;
	// addi r31,r22,14960
	r31.s64 = r22.s64 + 14960;
loc_820375C8:
	
	// lwz r11,52(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 52);
	// addi r10,r22,15352
	v_r10.s64 = r22.s64 + 15352;
	// lwz r9,-44(r31)
	v_r9.u64 = REX_LOAD_U32(r31.u32 + -44);
	// mulli r11,r11,120
	r11.s64 = static_cast<int64_t>(r11.u64 * static_cast<uint64_t>(120));
	// cmplwi cr6,r9,0
	cr6.compare<uint32_t>(v_r9.u32, 0, xer);
	// add r9,r11,r10
	v_r9.u64 = r11.u64 + v_r10.u64;
	// bne cr6,0x82037714
	if (!cr6.eq) goto loc_82037714;
	// lfs f0,-40(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + -40);
	f0.f64 = double(temp.f32);
	// lfs f13,-11648(r19)
	temp.u32 = REX_LOAD_U32(r19.u32 + -11648);
	v_f13.f64 = double(temp.f32);
	// fsubs f0,f0,f13
	f0.f64 = double(float(f0.f64 - v_f13.f64));
	// fsel f0,f0,f0,f24
	f0.f64 = f0.f64 >= 0.0 ? f0.f64 : f24.f64;
	// stfs f0,-40(r31)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r31.u32 + -40, temp.u32);
	// fcmpu cr6,f0,f24
	cr6.compare(f0.f64, f24.f64);
	// bgt cr6,0x82037714
	if (cr6.gt) goto loc_82037714;
	// lwz r10,15836(r22)
	v_r10.u64 = REX_LOAD_U32(r22.u32 + 15836);
	// mr r11,r20
	r11.u64 = r20.u64;
	// cmpwi cr6,r10,0
	cr6.compare<int32_t>(v_r10.s32, 0, xer);
	// bne cr6,0x82037638
	if (!cr6.eq) goto loc_82037638;
	// lbz r10,0(r31)
	v_r10.u64 = REX_LOAD_U8(r31.u32 + 0);
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(v_r10.u32, 0, xer);
	// beq cr6,0x82037624
	if (cr6.eq) goto loc_82037624;
	// stb r20,0(r31)
	REX_STORE_U8(r31.u32 + 0, r20.u8);
	// b 0x82037638
	goto loc_82037638;
loc_82037624:
	// lwz r10,48(r9)
	v_r10.u64 = REX_LOAD_U32(v_r9.u32 + 48);
	// cmpwi cr6,r10,0
	cr6.compare<int32_t>(v_r10.s32, 0, xer);
	// ble cr6,0x8203763c
	if (!cr6.gt) goto loc_8203763C;
	// addi r10,r10,-1
	v_r10.s64 = v_r10.s64 + -1;
	// stw r10,48(r9)
	REX_STORE_U32(v_r9.u32 + 48, v_r10.u32);
loc_82037638:
	// li r11,1
	r11.s64 = 1;
loc_8203763C:
	// clrlwi r11,r11,24
	r11.u64 = r11.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037714
	if (cr6.eq) goto loc_82037714;
	// li r3,248
	v_r3.s64 = 248;
	// bl 0x820e8130
	ctx.lr = 0x82037650;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820E8130(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(v_r3.u32, 0, xer);
	// beq cr6,0x82037690
	if (cr6.eq) goto loc_82037690;
	// lwz r8,44(r31)
	v_r8.u64 = REX_LOAD_U32(r31.u32 + 44);
	// addi r6,r22,14880
	v_r6.s64 = r22.s64 + 14880;
	// lwz r11,48(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 48);
	// addi r29,r22,14880
	r29.s64 = r22.s64 + 14880;
	// rlwinm r5,r8,3,0,28
	v_r5.u64 = __builtin_rotateleft64(v_r8.u32 | (v_r8.u64 << 32), 3) & 0xFFFFFFF8;
	// lwz r10,60(r31)
	v_r10.u64 = REX_LOAD_U32(r31.u32 + 60);
	// rlwinm r11,r11,3,0,28
	r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 3) & 0xFFFFFFF8;
	// lwz r9,56(r31)
	v_r9.u64 = REX_LOAD_U32(r31.u32 + 56);
	// mr r8,r30
	v_r8.u64 = r30.u64;
	// lwz r7,52(r31)
	v_r7.u64 = REX_LOAD_U32(r31.u32 + 52);
	// addi r4,r31,-32
	v_r4.s64 = r31.s64 + -32;
	// lwzx r5,r5,r29
	v_r5.u64 = REX_LOAD_U32(v_r5.u32 + r29.u32);
	// lwzx r6,r11,r6
	v_r6.u64 = REX_LOAD_U32(r11.u32 + v_r6.u32);
	// bl 0x82051448
	ctx.lr = 0x82037690;
	ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82051448(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_82037690:
	// lbz r11,15877(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 15877);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x8203770c
	if (cr6.eq) goto loc_8203770C;
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// cmpwi cr6,r11,1
	cr6.compare<int32_t>(r11.s32, 1, xer);
	// ble cr6,0x8203770c
	if (!cr6.gt) goto loc_8203770C;
	// lwz r11,60(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 60);
	// cmplwi cr6,r11,3
	cr6.compare<uint32_t>(r11.u32, 3, xer);
	// bgt cr6,0x82037710
	if (cr6.gt) goto loc_82037710;
	// lis r12,-32253
	r12.s64 = -2113732608;
	// addi r12,r12,30412
	r12.s64 = r12.s64 + 30412;
	// rlwinm r0,r11,2,0,29
	r0.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 2) & 0xFFFFFFFC;
	// lwzx r0,r12,r0
	r0.u64 = REX_LOAD_U32(r12.u32 + r0.u32);
	// mtctr r0
	ctr.u64 = r0.u64;
	// bctr 
	switch (r11.u32) {
	case 0:
		goto loc_820376DC;
	case 1:
		goto loc_820376E8;
	case 2:
		goto loc_820376F4;
	case 3:
		goto loc_82037700;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_820376DC:
	// li r3,86
	v_r3.s64 = 86;
	// bl 0x820787f8
	ctx.lr = 0x820376E4;
	ctx.r3 = v_r3;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// b 0x82037710
	goto loc_82037710;
loc_820376E8:
	// li r3,87
	v_r3.s64 = 87;
	// bl 0x820787f8
	ctx.lr = 0x820376F0;
	ctx.r3 = v_r3;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// b 0x82037710
	goto loc_82037710;
loc_820376F4:
	// li r3,88
	v_r3.s64 = 88;
	// bl 0x820787f8
	ctx.lr = 0x820376FC;
	ctx.r3 = v_r3;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// b 0x82037710
	goto loc_82037710;
loc_82037700:
	// li r3,89
	v_r3.s64 = 89;
	// bl 0x820787f8
	ctx.lr = 0x82037708;
	ctx.r3 = v_r3;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// b 0x82037710
	goto loc_82037710;
loc_8203770C:
	// li r28,1
	r28.s64 = 1;
loc_82037710:
	// li r29,1
	r29.s64 = 1;
loc_82037714:
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// addi r30,r30,1
	r30.s64 = r30.s64 + 1;
	// addi r31,r31,108
	r31.s64 = r31.s64 + 108;
	// cmpw cr6,r30,r11
	cr6.compare<int32_t>(r30.s32, r11.s32, xer);
	// blt cr6,0x820375c8
	if (cr6.lt) goto loc_820375C8;
loc_82037728:
	// clrlwi r31,r29,24
	r31.u64 = r29.u32 & 0xFF;
	// cmplwi cr6,r31,0
	cr6.compare<uint32_t>(r31.u32, 0, xer);
	// beq cr6,0x8203773c
	if (cr6.eq) goto loc_8203773C;
	// li r11,1
	r11.s64 = 1;
	// stb r11,14861(r22)
	REX_STORE_U8(r22.u32 + 14861, r11.u8);
loc_8203773C:
	// clrlwi r11,r28,24
	r11.u64 = r28.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037750
	if (cr6.eq) goto loc_82037750;
	// li r3,3
	v_r3.s64 = 3;
	// bl 0x820787f8
	ctx.lr = 0x82037750;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_82037750:
	// cmplwi cr6,r31,0
	cr6.compare<uint32_t>(r31.u32, 0, xer);
	// beq cr6,0x820377e8
	if (cr6.eq) goto loc_820377E8;
	// lbz r11,14848(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 14848);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x820377e8
	if (cr6.eq) goto loc_820377E8;
	// lwz r11,15844(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 15844);
	// cmpwi cr6,r11,5
	cr6.compare<int32_t>(r11.s32, 5, xer);
	// bne cr6,0x820377e0
	if (!cr6.eq) goto loc_820377E0;
	// li r3,148
	v_r3.s64 = 148;
	// bl 0x820e8130
	ctx.lr = 0x82037778;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820E8130(ctx, base);
	v_r3 = ctx.r3;
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(v_r3.u32, 0, xer);
	// beq cr6,0x82037790
	if (cr6.eq) goto loc_82037790;
	// addi r4,r1,208
	v_r4.s64 = ctx.r1.s64 + 208;
	// stfs f24,208(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 208, temp.u32);
	// stfs f24,212(r1)
	temp.f32 = float(f24.f64);
	REX_STORE_U32(ctx.r1.u32 + 212, temp.u32);
	// bl 0x8204ad38
	ctx.lr = 0x82037790;
	ctx.r4 = v_r4;
	sub_8204AD38(ctx, base);
loc_82037790:
	// li r3,148
	v_r3.s64 = 148;
	// bl 0x820e8130
	ctx.lr = 0x82037798;
	ctx.r3 = v_r3;
	sub_820E8130(ctx, base);
	v_r3 = ctx.r3;
	// mr r31,r3
	r31.u64 = v_r3.u64;
	// cmplwi cr6,r31,0
	cr6.compare<uint32_t>(r31.u32, 0, xer);
	// beq cr6,0x820377b8
	if (cr6.eq) goto loc_820377B8;
	// addi r3,r1,112
	v_r3.s64 = ctx.r1.s64 + 112;
	// bl 0x8204a9c0
	ctx.lr = 0x820377AC;
	ctx.r3 = v_r3;
	sub_8204A9C0(ctx, base);
	v_r3 = ctx.r3;
	// mr r4,r3
	v_r4.u64 = v_r3.u64;
	// mr r3,r31
	v_r3.u64 = r31.u64;
	// bl 0x8204ad38
	ctx.lr = 0x820377B8;
	ctx.r3 = v_r3; ctx.r4 = v_r4;
	sub_8204AD38(ctx, base);
loc_820377B8:
	// li r3,148
	v_r3.s64 = 148;
	// bl 0x820e8130
	ctx.lr = 0x820377C0;
	ctx.r3 = v_r3;
	sub_820E8130(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9;
	// mr r31,r3
	r31.u64 = v_r3.u64;
	// cmplwi cr6,r31,0
	cr6.compare<uint32_t>(r31.u32, 0, xer);
	// beq cr6,0x820377e0
	if (cr6.eq) goto loc_820377E0;
	// addi r3,r1,112
	v_r3.s64 = ctx.r1.s64 + 112;
	// bl 0x8204a9c0
	ctx.lr = 0x820377D4;
	ctx.r3 = v_r3;
	sub_8204A9C0(ctx, base);
	v_r3 = ctx.r3;
	// mr r4,r3
	v_r4.u64 = v_r3.u64;
	// mr r3,r31
	v_r3.u64 = r31.u64;
	// bl 0x8204ad38
	ctx.lr = 0x820377E0;
	ctx.r3 = v_r3; ctx.r4 = v_r4;
	sub_8204AD38(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9;
loc_820377E0:
	// mr r11,r20
	r11.u64 = r20.u64;
	// stb r11,14848(r22)
	REX_STORE_U8(r22.u32 + 14848, r11.u8);
loc_820377E8:
	// lwz r11,15836(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 15836);
	// mr r29,r20
	r29.u64 = r20.u64;
	// lwz r31,15348(r22)
	r31.u64 = REX_LOAD_U32(r22.u32 + 15348);
	// lwz r3,4(r21)
	v_r3.u64 = REX_LOAD_U32(r21.u32 + 4);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// beq cr6,0x8203781c
	if (cr6.eq) goto loc_8203781C;
	// cmpwi cr6,r11,2
	cr6.compare<int32_t>(r11.s32, 2, xer);
	// bne cr6,0x820378bc
	if (!cr6.eq) goto loc_820378BC;
	// lfs f0,14840(r22)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r22.u32 + 14840);
	f0.f64 = double(temp.f32);
	// lfs f13,15848(r22)
	temp.u32 = REX_LOAD_U32(r22.u32 + 15848);
	v_f13.f64 = double(temp.f32);
	// fcmpu cr6,f0,f13
	cr6.compare(f0.f64, v_f13.f64);
	// blt cr6,0x820378bc
	if (cr6.lt) goto loc_820378BC;
	// b 0x820378b8
	goto loc_820378B8;
loc_8203781C:
	// mr r10,r3
	v_r10.u64 = v_r3.u64;
	// mr r8,r20
	v_r8.u64 = r20.u64;
	// lwz r11,0(r10)
	r11.u64 = REX_LOAD_U32(v_r10.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// beq cr6,0x82037854
	if (cr6.eq) goto loc_82037854;
loc_82037830:
	
	// lwz r11,36(r11)
	r11.u64 = REX_LOAD_U32(r11.u32 + 36);
	// clrlwi r11,r11,31
	r11.u64 = r11.u32 & 0x1;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x82037844
	if (!cr6.eq) goto loc_82037844;
	// addi r8,r8,1
	v_r8.s64 = v_r8.s64 + 1;
loc_82037844:
	// lwz r10,4(r10)
	v_r10.u64 = REX_LOAD_U32(v_r10.u32 + 4);
	// lwz r11,0(r10)
	r11.u64 = REX_LOAD_U32(v_r10.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x82037830
	if (!cr6.eq) goto loc_82037830;
loc_82037854:
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x82037888
	if (!cr6.gt) goto loc_82037888;
	// rotlwi r11,r11,0
	r11.u64 = __builtin_rotateleft32(r11.u32, 0);
	// addi r10,r22,14960
	v_r10.s64 = r22.s64 + 14960;
loc_82037868:
	
	// lbz r9,0(r10)
	v_r9.u64 = REX_LOAD_U8(v_r10.u32 + 0);
	// cmplwi cr6,r9,0
	cr6.compare<uint32_t>(v_r9.u32, 0, xer);
	// beq cr6,0x82037878
	if (cr6.eq) goto loc_82037878;
	// addi r8,r8,1
	v_r8.s64 = v_r8.s64 + 1;
loc_82037878:
	// addi r11,r11,-1
	r11.s64 = r11.s64 + -1;
	// addi r10,r10,108
	v_r10.s64 = v_r10.s64 + 108;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037868
	if (!cr6.eq) goto loc_82037868;
loc_82037888:
	// cmpwi cr6,r31,0
	cr6.compare<int32_t>(r31.s32, 0, xer);
	// ble cr6,0x820378b0
	if (!cr6.gt) goto loc_820378B0;
	// mr r11,r31
	r11.u64 = r31.u64;
	// addi r10,r22,15400
	v_r10.s64 = r22.s64 + 15400;
loc_82037898:
	
	// lwz r9,0(r10)
	v_r9.u64 = REX_LOAD_U32(v_r10.u32 + 0);
	// addi r11,r11,-1
	r11.s64 = r11.s64 + -1;
	// addi r10,r10,120
	v_r10.s64 = v_r10.s64 + 120;
	// add r8,r9,r8
	v_r8.u64 = v_r9.u64 + v_r8.u64;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037898
	if (!cr6.eq) goto loc_82037898;
loc_820378B0:
	// cmpwi cr6,r8,0
	cr6.compare<int32_t>(v_r8.s32, 0, xer);
	// bgt cr6,0x820378bc
	if (cr6.gt) goto loc_820378BC;
loc_820378B8:
	// li r29,1
	r29.s64 = 1;
loc_820378BC:
	// lbz r11,15877(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 15877);
	// mr r30,r20
	r30.u64 = r20.u64;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037a44
	if (cr6.eq) goto loc_82037A44;
	// cmpwi cr6,r31,1
	cr6.compare<int32_t>(r31.s32, 1, xer);
	// ble cr6,0x82037a44
	if (!cr6.gt) goto loc_82037A44;
	// lwz r11,15836(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 15836);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x82037a44
	if (!cr6.eq) goto loc_82037A44;
	// mr r5,r20
	v_r5.u64 = r20.u64;
	// mr r4,r20
	v_r4.u64 = r20.u64;
	// mr r7,r20
	v_r7.u64 = r20.u64;
	// cmpwi cr6,r31,0
	cr6.compare<int32_t>(r31.s32, 0, xer);
	// ble cr6,0x82037a44
	if (!cr6.gt) goto loc_82037A44;
	// addi r8,r22,15352
	v_r8.s64 = r22.s64 + 15352;
loc_820378F8:
	
	// lwz r11,48(r8)
	r11.u64 = REX_LOAD_U32(v_r8.u32 + 48);
	// mr r6,r20
	v_r6.u64 = r20.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bgt cr6,0x8203799c
	if (cr6.gt) goto loc_8203799C;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// lwz r11,0(r9)
	r11.u64 = REX_LOAD_U32(v_r9.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// beq cr6,0x82037958
	if (cr6.eq) goto loc_82037958;
loc_82037918:
	
	// lwz r10,36(r11)
	v_r10.u64 = REX_LOAD_U32(r11.u32 + 36);
	// clrlwi r10,r10,31
	v_r10.u64 = v_r10.u32 & 0x1;
	// cmpwi cr6,r10,0
	cr6.compare<int32_t>(v_r10.s32, 0, xer);
	// bne cr6,0x82037940
	if (!cr6.eq) goto loc_82037940;
	// lwz r11,164(r11)
	r11.u64 = REX_LOAD_U32(r11.u32 + 164);
	// addi r10,r22,15352
	v_r10.s64 = r22.s64 + 15352;
	// mulli r11,r11,120
	r11.s64 = static_cast<int64_t>(r11.u64 * static_cast<uint64_t>(120));
	// add r11,r11,r10
	r11.u64 = r11.u64 + v_r10.u64;
	// cmplw cr6,r11,r8
	cr6.compare<uint32_t>(r11.u32, v_r8.u32, xer);
	// beq cr6,0x82037954
	if (cr6.eq) goto loc_82037954;
loc_82037940:
	// lwz r9,4(r9)
	v_r9.u64 = REX_LOAD_U32(v_r9.u32 + 4);
	// lwz r11,0(r9)
	r11.u64 = REX_LOAD_U32(v_r9.u32 + 0);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x82037918
	if (!cr6.eq) goto loc_82037918;
	// b 0x82037958
	goto loc_82037958;
loc_82037954:
	// li r6,1
	v_r6.s64 = 1;
loc_82037958:
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// mr r10,r20
	v_r10.u64 = r20.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x820379a0
	if (!cr6.gt) goto loc_820379A0;
	// addi r11,r22,14960
	r11.s64 = r22.s64 + 14960;
loc_8203796C:
	
	// lwz r9,52(r11)
	v_r9.u64 = REX_LOAD_U32(r11.u32 + 52);
	// cmpw cr6,r9,r7
	cr6.compare<int32_t>(v_r9.s32, v_r7.s32, xer);
	// bne cr6,0x82037984
	if (!cr6.eq) goto loc_82037984;
	// lbz r9,0(r11)
	v_r9.u64 = REX_LOAD_U8(r11.u32 + 0);
	// cmplwi cr6,r9,0
	cr6.compare<uint32_t>(v_r9.u32, 0, xer);
	// bne cr6,0x8203799c
	if (!cr6.eq) goto loc_8203799C;
loc_82037984:
	// lwz r9,14912(r22)
	v_r9.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// addi r10,r10,1
	v_r10.s64 = v_r10.s64 + 1;
	// addi r11,r11,108
	r11.s64 = r11.s64 + 108;
	// cmpw cr6,r10,r9
	cr6.compare<int32_t>(v_r10.s32, v_r9.s32, xer);
	// blt cr6,0x8203796c
	if (cr6.lt) goto loc_8203796C;
	// b 0x820379a0
	goto loc_820379A0;
loc_8203799C:
	// li r6,1
	v_r6.s64 = 1;
loc_820379A0:
	// clrlwi r11,r6,24
	r11.u64 = v_r6.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x820379b4
	if (cr6.eq) goto loc_820379B4;
	// mr r4,r7
	v_r4.u64 = v_r7.u64;
	// addi r5,r5,1
	v_r5.s64 = v_r5.s64 + 1;
loc_820379B4:
	// addi r7,r7,1
	v_r7.s64 = v_r7.s64 + 1;
	// addi r8,r8,120
	v_r8.s64 = v_r8.s64 + 120;
	// cmpw cr6,r7,r31
	cr6.compare<int32_t>(v_r7.s32, r31.s32, xer);
	// blt cr6,0x820378f8
	if (cr6.lt) goto loc_820378F8;
	// cmpwi cr6,r5,1
	cr6.compare<int32_t>(v_r5.s32, 1, xer);
	// bne cr6,0x82037a44
	if (!cr6.eq) goto loc_82037A44;
	// mulli r11,r4,120
	r11.s64 = static_cast<int64_t>(v_r4.u64 * static_cast<uint64_t>(120));
	// addi r10,r22,15352
	v_r10.s64 = r22.s64 + 15352;
	// li r9,1
	v_r9.s64 = 1;
	// add r8,r11,r10
	v_r8.u64 = r11.u64 + v_r10.u64;
	// mr r11,r20
	r11.u64 = r20.u64;
	// addi r10,r22,15392
	v_r10.s64 = r22.s64 + 15392;
loc_820379E4:
	
	// cmpw cr6,r11,r4
	cr6.compare<int32_t>(r11.s32, v_r4.s32, xer);
	// beq cr6,0x82037a00
	if (cr6.eq) goto loc_82037A00;
	// ld r7,0(r10)
	v_r7.u64 = REX_LOAD_U64(v_r10.u32 + 0);
	// ld r6,40(r8)
	v_r6.u64 = REX_LOAD_U64(v_r8.u32 + 40);
	// cmpd cr6,r7,r6
	cr6.compare<int64_t>(v_r7.s64, v_r6.s64, xer);
	// blt cr6,0x82037a00
	if (cr6.lt) goto loc_82037A00;
	// mr r9,r20
	v_r9.u64 = r20.u64;
loc_82037A00:
	// addi r11,r11,1
	r11.s64 = r11.s64 + 1;
	// addi r10,r10,120
	v_r10.s64 = v_r10.s64 + 120;
	// cmpw cr6,r11,r31
	cr6.compare<int32_t>(r11.s32, r31.s32, xer);
	// blt cr6,0x820379e4
	if (cr6.lt) goto loc_820379E4;
	// clrlwi r11,r9,24
	r11.u64 = v_r9.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037a28
	if (cr6.eq) goto loc_82037A28;
	// li r30,1
	r30.s64 = 1;
	// li r29,1
	r29.s64 = 1;
	// b 0x82037a44
	goto loc_82037A44;
loc_82037A28:
	// lbz r11,14860(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 14860);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037a44
	if (!cr6.eq) goto loc_82037A44;
	// li r3,90
	v_r3.s64 = 90;
	// bl 0x820787f8
	ctx.lr = 0x82037A3C;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9;
	// li r11,1
	r11.s64 = 1;
	// stb r11,14860(r22)
	REX_STORE_U8(r22.u32 + 14860, r11.u8);
loc_82037A44:
	// lbz r11,14849(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 14849);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037a54
	if (cr6.eq) goto loc_82037A54;
	// li r29,1
	r29.s64 = 1;
loc_82037A54:
	// clrlwi r11,r29,24
	r11.u64 = r29.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037abc
	if (cr6.eq) goto loc_82037ABC;
	// li r11,1
	r11.s64 = 1;
	// lbz r10,14852(r22)
	v_r10.u64 = REX_LOAD_U8(r22.u32 + 14852);
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(v_r10.u32, 0, xer);
	// stb r11,14851(r22)
	REX_STORE_U8(r22.u32 + 14851, r11.u8);
	// bne cr6,0x82037aa0
	if (!cr6.eq) goto loc_82037AA0;
	// clrlwi r11,r30,24
	r11.u64 = r30.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037a88
	if (cr6.eq) goto loc_82037A88;
	// li r3,81
	v_r3.s64 = 81;
	// bl 0x820787f8
	ctx.lr = 0x82037A88;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_82037A88:
	// li r3,1
	v_r3.s64 = 1;
	// bl 0x820787f8
	ctx.lr = 0x82037A90;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820787F8(ctx, base);
	// li r3,37
	v_r3.s64 = 37;
	// bl 0x820787f8
	ctx.lr = 0x82037A98;
	ctx.r3 = v_r3;
	sub_820787F8(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// li r11,1
	r11.s64 = 1;
	// stb r11,14852(r22)
	REX_STORE_U8(r22.u32 + 14852, r11.u8);
loc_82037AA0:
	// lbz r11,14850(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 14850);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037abc
	if (cr6.eq) goto loc_82037ABC;
	// bl 0x82036358
	ctx.lr = 0x82037AB0;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82036358(ctx, base);
	// bl 0x820364d0
	ctx.lr = 0x82037AB4;
	sub_820364D0(ctx, base);
	// li r3,2
	v_r3.s64 = 2;
	// bl 0x82034408
	ctx.lr = 0x82037ABC;
	ctx.r3 = v_r3;
	sub_82034408(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r4 = ctx.r4; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9;
loc_82037ABC:
	// lwz r11,14912(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// mr r10,r20
	v_r10.u64 = r20.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x82037af8
	if (!cr6.gt) goto loc_82037AF8;
	// addi r11,r22,14980
	r11.s64 = r22.s64 + 14980;
loc_82037AD0:
	
	// lfs f0,0(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f0.f64 = double(temp.f32);
	// addi r10,r10,1
	v_r10.s64 = v_r10.s64 + 1;
	// lfs f13,-11648(r19)
	temp.u32 = REX_LOAD_U32(r19.u32 + -11648);
	v_f13.f64 = double(temp.f32);
	// fsubs f0,f0,f13
	f0.f64 = double(float(f0.f64 - v_f13.f64));
	// fsel f0,f0,f0,f24
	f0.f64 = f0.f64 >= 0.0 ? f0.f64 : f24.f64;
	// stfs f0,0(r11)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r11.u32 + 0, temp.u32);
	// lwz r9,14912(r22)
	v_r9.u64 = REX_LOAD_U32(r22.u32 + 14912);
	// addi r11,r11,108
	r11.s64 = r11.s64 + 108;
	// cmpw cr6,r10,r9
	cr6.compare<int32_t>(v_r10.s32, v_r9.s32, xer);
	// blt cr6,0x82037ad0
	if (cr6.lt) goto loc_82037AD0;
loc_82037AF8:
	// bl 0x8204fad8
	ctx.lr = 0x82037AFC;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8204FAD8(ctx, base);
	// bl 0x82030a98
	ctx.lr = 0x82037B00;
	sub_82030A98(ctx, base);
	// bl 0x8205c8f8
	ctx.lr = 0x82037B04;
	sub_8205C8F8(ctx, base);
	// bl 0x82067390
	ctx.lr = 0x82037B08;
	sub_82067390(ctx, base);
	// bl 0x8205ca98
	ctx.lr = 0x82037B0C;
	sub_8205CA98(ctx, base);
loc_82037B0C:
	// bl 0x82033610
	ctx.lr = 0x82037B10;
	sub_82033610(ctx, base);
	v_r3 = ctx.r3; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x82037c0c
	if (cr6.eq) goto loc_82037C0C;
	// lbz r11,15877(r22)
	r11.u64 = REX_LOAD_U8(r22.u32 + 15877);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037c0c
	if (!cr6.eq) goto loc_82037C0C;
	// lwz r11,15012(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 15012);
	// addi r9,r22,15352
	v_r9.s64 = r22.s64 + 15352;
	// mulli r10,r11,120
	v_r10.s64 = static_cast<int64_t>(r11.u64 * static_cast<uint64_t>(120));
	// lwz r11,15844(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 15844);
	// cmpwi cr6,r11,5
	cr6.compare<int32_t>(r11.s32, 5, xer);
	// add r31,r10,r9
	r31.u64 = v_r10.u64 + v_r9.u64;
	// bne cr6,0x82037b70
	if (!cr6.eq) goto loc_82037B70;
	// lbz r11,96(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 96);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037bf0
	if (!cr6.eq) goto loc_82037BF0;
	// lbz r11,97(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 97);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037bf0
	if (!cr6.eq) goto loc_82037BF0;
	// lwz r11,100(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 100);
	// cmpwi cr6,r11,30
	cr6.compare<int32_t>(r11.s32, 30, xer);
	// blt cr6,0x82037bf0
	if (cr6.lt) goto loc_82037BF0;
	// li r4,2
	v_r4.s64 = 2;
	// b 0x82037be4
	goto loc_82037BE4;
loc_82037B70:
	// cmpwi cr6,r11,3
	cr6.compare<int32_t>(r11.s32, 3, xer);
	// bne cr6,0x82037bcc
	if (!cr6.eq) goto loc_82037BCC;
	// lbz r11,96(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 96);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037bac
	if (!cr6.eq) goto loc_82037BAC;
	// lbz r11,97(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 97);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x82037bac
	if (!cr6.eq) goto loc_82037BAC;
	// lwz r11,14864(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14864);
	// cmpwi cr6,r11,8
	cr6.compare<int32_t>(r11.s32, 8, xer);
	// blt cr6,0x82037bac
	if (cr6.lt) goto loc_82037BAC;
	// li r5,0
	v_r5.s64 = 0;
	// li r4,64
	v_r4.s64 = 64;
	// li r3,0
	v_r3.s64 = 0;
	// bl 0x82034ca0
	ctx.lr = 0x82037BAC;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82034CA0(ctx, base);
	v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_82037BAC:
	// ld r11,40(r31)
	r11.u64 = REX_LOAD_U64(r31.u32 + 40);
	// cmpdi cr6,r11,1000
	cr6.compare<int64_t>(r11.s64, 1000, xer);
	// blt cr6,0x82037bf0
	if (cr6.lt) goto loc_82037BF0;
	// lwz r11,64(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 64);
	// cmpwi cr6,r11,1
	cr6.compare<int32_t>(r11.s32, 1, xer);
	// bne cr6,0x82037bf0
	if (!cr6.eq) goto loc_82037BF0;
	// li r4,256
	v_r4.s64 = 256;
	// b 0x82037be4
	goto loc_82037BE4;
loc_82037BCC:
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x82037bf0
	if (!cr6.eq) goto loc_82037BF0;
	// lwz r11,14976(r22)
	r11.u64 = REX_LOAD_U32(r22.u32 + 14976);
	// cmpwi cr6,r11,75
	cr6.compare<int32_t>(r11.s32, 75, xer);
	// blt cr6,0x82037bf0
	if (cr6.lt) goto loc_82037BF0;
	// li r4,512
	v_r4.s64 = 512;
loc_82037BE4:
	// li r5,0
	v_r5.s64 = 0;
	// li r3,0
	v_r3.s64 = 0;
	// bl 0x82034ca0
	ctx.lr = 0x82037BF0;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82034CA0(ctx, base);
	v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_82037BF0:
	// lwz r11,64(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 64);
	// cmpwi cr6,r11,500
	cr6.compare<int32_t>(r11.s32, 500, xer);
	// blt cr6,0x82037c0c
	if (cr6.lt) goto loc_82037C0C;
	// li r5,0
	v_r5.s64 = 0;
	// li r4,128
	v_r4.s64 = 128;
	// li r3,0
	v_r3.s64 = 0;
	// bl 0x82034ca0
	ctx.lr = 0x82037C0C;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_82034CA0(ctx, base);
	v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_82037C0C:
	// addi r1,r1,640
	ctx.r1.s64 = ctx.r1.s64 + 640;
	// addi r12,r1,-112
	r12.s64 = ctx.r1.s64 + -112;
	// bl 0x8212e8d0
	ctx.lr = 0x82037C18;
	// b 0x8212e964
	ctx.r9 = v_r9; ctx.r10 = v_r10;
	return;

}
#endif  // GW_BIG_NATIVE

// ---------------------------------------------------------------------------
// The score display (guest 0x8207CA48): the players' scores, times and
// standings drawn as text (1,209 instructions calling 20 functions). As 0x820366A0 above: the generated code with the volatile
// registers in locals, synchronised with the context only where a call or
// the return can see them.
extern "C" void sub_82033600(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82034588(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205D740(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82076130(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820762F0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820763D0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820764C0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820765A8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82076778(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82076E40(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820770E8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8207C3A0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8207C530(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8207C790(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8207E238(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8207E248(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820826D8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8208BBF8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8208BCF8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82093600(PPCContext& ctx, uint8_t* base);
#ifdef GW_BIG_NATIVE  // (make BIG_NATIVE=y): the generated code with registers in locals; no faster on the PC, part of it untested
extern "C" void sub_8207CA48(PPCContext& ctx, uint8_t* base) {
	PPCRegister v_f1{}; PPCRegister v_f2{}; PPCRegister v_f3{}; PPCRegister v_f4{}; PPCRegister v_f5{}; PPCRegister v_f10{}; PPCRegister v_f11{}; PPCRegister v_f12{}; PPCRegister v_f13 = ctx.f13; PPCRegister v_r3{}; PPCRegister v_r4{}; PPCRegister v_r5{}; PPCRegister v_r6{}; PPCRegister v_r7{}; PPCRegister v_r8{}; PPCRegister v_r9{}; PPCRegister v_r10{}; PPCRegister v_r12 = ctx.r12;

	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12 = v_r12;
	PPCRegister r15{};
	PPCRegister r16{};
	PPCRegister r17{};
	PPCRegister r18{};
	PPCRegister r19{};
	PPCRegister r20{};
	PPCRegister r21{};
	PPCRegister r22{};
	PPCRegister r23{};
	PPCRegister r24{};
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f0{};
	PPCRegister f18{};
	PPCRegister f19{};
	PPCRegister f20{};
	PPCRegister f21{};
	PPCRegister f22{};
	PPCRegister f23{};
	PPCRegister f24{};
	PPCRegister f25{};
	PPCRegister f26{};
	PPCRegister f27{};
	PPCRegister f28{};
	PPCRegister f29{};
	PPCRegister f30{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	r12.u64 = ctx.lr;
	// bl 0x8212e904
	ctx.lr = 0x8207CA50;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e870
	ctx.lr = 0x8207CA58;
	// stwu r1,-1984(r1)
	ea = -1984 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lis r11,-32230
	r11.s64 = -2112225280;
	// li r15,0
	r15.s64 = 0;
	// addi r31,r11,-27536
	r31.s64 = r11.s64 + -27536;
	// lwz r8,15348(r31)
	v_r8.u64 = REX_LOAD_U32(r31.u32 + 15348);
	// cmpwi cr6,r8,1
	cr6.compare<int32_t>(v_r8.s32, 1, xer);
	// ble cr6,0x8207d024
	if (!cr6.gt) goto loc_8207D024;
	// lwz r11,15860(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15860);
	// li r16,1
	r16.s64 = 1;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bgt cr6,0x8207ca98
	if (cr6.gt) goto loc_8207CA98;
	// lwz r11,15868(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15868);
	// clrlwi r11,r11,31
	r11.u64 = r11.u32 & 0x1;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// mr r11,r15
	r11.u64 = r15.u64;
	// bne cr6,0x8207ca9c
	if (!cr6.eq) goto loc_8207CA9C;
loc_8207CA98:
	// mr r11,r16
	r11.u64 = r16.u64;
loc_8207CA9C:
	// ld r19,15392(r31)
	r19.u64 = REX_LOAD_U64(r31.u32 + 15392);
	// clrlwi r7,r11,24
	v_r7.u64 = r11.u32 & 0xFF;
	// mr r18,r15
	r18.u64 = r15.u64;
	// mr r10,r16
	v_r10.u64 = r16.u64;
	// cmpwi cr6,r8,1
	cr6.compare<int32_t>(v_r8.s32, 1, xer);
	// ble cr6,0x8207cae8
	if (!cr6.gt) goto loc_8207CAE8;
	// addi r9,r31,15512
	v_r9.s64 = r31.s64 + 15512;
loc_8207CAB8:
	
	// ld r11,0(r9)
	r11.u64 = REX_LOAD_U64(v_r9.u32 + 0);
	// cmpd cr6,r11,r19
	cr6.compare<int64_t>(r11.s64, r19.s64, xer);
	// ble cr6,0x8207cad0
	if (!cr6.gt) goto loc_8207CAD0;
	// mr r18,r10
	r18.u64 = v_r10.u64;
	// mr r19,r11
	r19.u64 = r11.u64;
	// b 0x8207cad8
	goto loc_8207CAD8;
loc_8207CAD0:
	// bne cr6,0x8207cad8
	if (!cr6.eq) goto loc_8207CAD8;
	// li r18,-1
	r18.s64 = -1;
loc_8207CAD8:
	// addi r10,r10,1
	v_r10.s64 = v_r10.s64 + 1;
	// addi r9,r9,120
	v_r9.s64 = v_r9.s64 + 120;
	// cmpw cr6,r10,r8
	cr6.compare<int32_t>(v_r10.s32, v_r8.s32, xer);
	// blt cr6,0x8207cab8
	if (cr6.lt) goto loc_8207CAB8;
loc_8207CAE8:
	// cmpwi cr6,r8,0
	cr6.compare<int32_t>(v_r8.s32, 0, xer);
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// mr r26,r15
	r26.u64 = r15.u64;
	// lfs f19,3432(r8)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r8.u32 + 3432);
	f19.f64 = double(temp.f32);
	// addi r17,r11,18740
	r17.s64 = r11.s64 + 18740;
	// lfs f20,1788(r9)
	temp.u32 = REX_LOAD_U32(v_r9.u32 + 1788);
	f20.f64 = double(temp.f32);
	// lfs f18,18660(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 18660);
	f18.f64 = double(temp.f32);
	// ble cr6,0x8207ce1c
	if (!cr6.gt) goto loc_8207CE1C;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// clrlwi r20,r7,24
	r20.u64 = v_r7.u32 & 0xFF;
	// addi r21,r11,18792
	r21.s64 = r11.s64 + 18792;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lis r30,-32256
	r30.s64 = -2113929216;
	// addi r24,r11,18788
	r24.s64 = r11.s64 + 18788;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lis r3,-32256
	v_r3.s64 = -2113929216;
	// lis r4,-32256
	v_r4.s64 = -2113929216;
	// lis r5,-32256
	v_r5.s64 = -2113929216;
	// lfs f25,3208(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 3208);
	f25.f64 = double(temp.f32);
	// lis r6,-32256
	v_r6.s64 = -2113929216;
	// lis r7,-32256
	v_r7.s64 = -2113929216;
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// lfs f26,3352(r3)
	temp.u32 = REX_LOAD_U32(v_r3.u32 + 3352);
	f26.f64 = double(temp.f32);
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// lfs f27,1804(r4)
	temp.u32 = REX_LOAD_U32(v_r4.u32 + 1804);
	f27.f64 = double(temp.f32);
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lfs f28,1732(r5)
	temp.u32 = REX_LOAD_U32(v_r5.u32 + 1732);
	f28.f64 = double(temp.f32);
	// addi r23,r11,3284
	r23.s64 = r11.s64 + 3284;
	// lfs f31,3420(r6)
	temp.u32 = REX_LOAD_U32(v_r6.u32 + 3420);
	f31.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f30,5140(r7)
	temp.u32 = REX_LOAD_U32(v_r7.u32 + 5140);
	f30.f64 = double(temp.f32);
	// lfs f21,3408(r8)
	temp.u32 = REX_LOAD_U32(v_r8.u32 + 3408);
	f21.f64 = double(temp.f32);
	// addi r27,r31,15392
	r27.s64 = r31.s64 + 15392;
	// lfs f22,18784(r9)
	temp.u32 = REX_LOAD_U32(v_r9.u32 + 18784);
	f22.f64 = double(temp.f32);
	// addi r22,r11,7528
	r22.s64 = r11.s64 + 7528;
	// lfs f23,3992(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3992);
	f23.f64 = double(temp.f32);
loc_8207CB84:
	
	// lwz r11,-40(r27)
	r11.u64 = REX_LOAD_U32(r27.u32 + -40);
	// addi r28,r27,-40
	r28.s64 = r27.s64 + -40;
	// addi r3,r1,112
	v_r3.s64 = ctx.r1.s64 + 112;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// ble cr6,0x8207cbac
	if (!cr6.gt) goto loc_8207CBAC;
	// lwz r11,-36(r27)
	r11.u64 = REX_LOAD_U32(r27.u32 + -36);
	// addi r10,r31,15016
	v_r10.s64 = r31.s64 + 15016;
	// mulli r11,r11,108
	r11.s64 = static_cast<int64_t>(r11.u64 * static_cast<uint64_t>(108));
	// lwzx r4,r11,r10
	v_r4.u64 = REX_LOAD_U32(r11.u32 + v_r10.u32);
	// b 0x8207cbb0
	goto loc_8207CBB0;
loc_8207CBAC:
	// li r4,0
	v_r4.s64 = 0;
loc_8207CBB0:
	// bl 0x8207c3a0
	ctx.lr = 0x8207CBB4;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8207C3A0(ctx, base);
	// clrlwi r25,r26,31
	r25.u64 = r26.u32 & 0x1;
	// cmpwi cr6,r25,0
	cr6.compare<int32_t>(r25.s32, 0, xer);
	// beq cr6,0x8207cbc8
	if (cr6.eq) goto loc_8207CBC8;
	// lfs f24,4(r17)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r17.u32 + 4);
	f24.f64 = double(temp.f32);
	// b 0x8207cbcc
	goto loc_8207CBCC;
loc_8207CBC8:
	// lfs f24,0(r17)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r17.u32 + 0);
	f24.f64 = double(temp.f32);
loc_8207CBCC:
	// rlwinm r11,r26,0,30,30
	r11.u64 = __builtin_rotateleft64(r26.u32 | (r26.u64 << 32), 0) & 0x2;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// beq cr6,0x8207cbe0
	if (cr6.eq) goto loc_8207CBE0;
	// fmr f29,f23
	ctx.fpscr.disableFlushMode();
	f29.f64 = f23.f64;
	// b 0x8207cbe4
	goto loc_8207CBE4;
loc_8207CBE0:
	// fmr f29,f18
	ctx.fpscr.disableFlushMode();
	f29.f64 = f18.f64;
loc_8207CBE4:
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076778
	ctx.lr = 0x8207CBEC;
	ctx.r3 = v_r3;
	sub_82076778(ctx, base);
	// cmpwi cr6,r25,0
	cr6.compare<int32_t>(r25.s32, 0, xer);
	// bne cr6,0x8207ccf4
	if (!cr6.eq) goto loc_8207CCF4;
	// addi r5,r1,112
	v_r5.s64 = ctx.r1.s64 + 112;
	// addi r4,r1,240
	v_r4.s64 = ctx.r1.s64 + 240;
	// mr r3,r28
	v_r3.u64 = r28.u64;
	// bl 0x8207c790
	ctx.lr = 0x8207CC04;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_8207C790(ctx, base);
	v_r3 = ctx.r3;
	// mr r28,r3
	r28.u64 = v_r3.u64;
	// cmplwi cr6,r20,0
	cr6.compare<uint32_t>(r20.u32, 0, xer);
	// beq cr6,0x8207cc34
	if (cr6.eq) goto loc_8207CC34;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// lwz r6,8(r27)
	v_r6.u64 = REX_LOAD_U32(r27.u32 + 8);
	// mr r5,r24
	v_r5.u64 = r24.u64;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CC24;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(v_r3.u32, 0, xer);
	// beq cr6,0x8207cc30
	if (cr6.eq) goto loc_8207CC30;
	// stfs f20,24(r3)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f20.f64);
	REX_STORE_U32(v_r3.u32 + 24, temp.u32);
loc_8207CC30:
	// mr r28,r3
	r28.u64 = v_r3.u64;
loc_8207CC34:
	// lwz r11,24(r27)
	r11.u64 = REX_LOAD_U32(r27.u32 + 24);
	// extsw r3,r11
	v_r3.s64 = r11.s32;
	// bl 0x8205d740
	ctx.lr = 0x8207CC40;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// mr r6,r3
	v_r6.u64 = v_r3.u64;
	// mr r5,r22
	v_r5.u64 = r22.u64;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CC54;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// mr r30,r3
	r30.u64 = v_r3.u64;
	// cmplwi cr6,r30,0
	cr6.compare<uint32_t>(r30.u32, 0, xer);
	// beq cr6,0x8207cc64
	if (cr6.eq) goto loc_8207CC64;
	// stfs f22,36(r30)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f22.f64);
	REX_STORE_U32(r30.u32 + 36, temp.u32);
loc_8207CC64:
	// ld r3,0(r27)
	v_r3.u64 = REX_LOAD_U64(r27.u32 + 0);
	// bl 0x8205d740
	ctx.lr = 0x8207CC6C;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// mr r6,r3
	v_r6.u64 = v_r3.u64;
	// mr r5,r23
	v_r5.u64 = r23.u64;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CC80;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// mr r29,r3
	r29.u64 = v_r3.u64;
	// cmplwi cr6,r29,0
	cr6.compare<uint32_t>(r29.u32, 0, xer);
	// beq cr6,0x8207cc90
	if (cr6.eq) goto loc_8207CC90;
	// stfs f21,36(r29)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f21.f64);
	REX_STORE_U32(r29.u32 + 36, temp.u32);
loc_8207CC90:
	// cmpw cr6,r26,r18
	cr6.compare<int32_t>(r26.s32, r18.s32, xer);
	// bne cr6,0x8207cca0
	if (!cr6.eq) goto loc_8207CCA0;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// b 0x8207ccc0
	goto loc_8207CCC0;
loc_8207CCA0:
	// ld r11,0(r27)
	r11.u64 = REX_LOAD_U64(r27.u32 + 0);
	// cmpd cr6,r11,r19
	cr6.compare<int64_t>(r11.s64, r19.s64, xer);
	// bne cr6,0x8207ccd0
	if (!cr6.eq) goto loc_8207CCD0;
	// stfs f31,224(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 224, temp.u32);
	// addi r4,r1,224
	v_r4.s64 = ctx.r1.s64 + 224;
	// stfs f31,228(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 228, temp.u32);
	// stfs f31,232(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 232, temp.u32);
	// stfs f28,236(r1)
	temp.f32 = float(f28.f64);
	REX_STORE_U32(ctx.r1.u32 + 236, temp.u32);
loc_8207CCC0:
	// mr r5,r21
	v_r5.u64 = r21.u64;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CCCC;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// stfs f30,24(r3)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f30.f64);
	REX_STORE_U32(v_r3.u32 + 24, temp.u32);
loc_8207CCD0:
	// lfs f0,28(r28)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r28.u32 + 28);
	f0.f64 = double(temp.f32);
	// fadds f0,f0,f19
	f0.f64 = double(float(f0.f64 + f19.f64));
	// stfs f0,28(r28)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r28.u32 + 28, temp.u32);
	// lfs f0,28(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 28);
	f0.f64 = double(temp.f32);
	// stw r15,32(r30)
	REX_STORE_U32(r30.u32 + 32, r15.u32);
	// fadds f0,f0,f19
	f0.f64 = double(float(f0.f64 + f19.f64));
	// stfs f0,28(r30)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r30.u32 + 28, temp.u32);
	// stw r16,32(r29)
	REX_STORE_U32(r29.u32 + 32, r16.u32);
	// b 0x8207cde8
	goto loc_8207CDE8;
loc_8207CCF4:
	// cmpw cr6,r26,r18
	cr6.compare<int32_t>(r26.s32, r18.s32, xer);
	// bne cr6,0x8207cd04
	if (!cr6.eq) goto loc_8207CD04;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// b 0x8207cd24
	goto loc_8207CD24;
loc_8207CD04:
	// ld r11,0(r27)
	r11.u64 = REX_LOAD_U64(r27.u32 + 0);
	// cmpd cr6,r11,r19
	cr6.compare<int64_t>(r11.s64, r19.s64, xer);
	// bne cr6,0x8207cd34
	if (!cr6.eq) goto loc_8207CD34;
	// stfs f31,144(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 144, temp.u32);
	// addi r4,r1,144
	v_r4.s64 = ctx.r1.s64 + 144;
	// stfs f31,148(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 148, temp.u32);
	// stfs f31,152(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 152, temp.u32);
	// stfs f28,156(r1)
	temp.f32 = float(f28.f64);
	REX_STORE_U32(ctx.r1.u32 + 156, temp.u32);
loc_8207CD24:
	// mr r5,r21
	v_r5.u64 = r21.u64;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CD30;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// stfs f30,28(r3)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f30.f64);
	REX_STORE_U32(v_r3.u32 + 28, temp.u32);
loc_8207CD34:
	// ld r3,0(r27)
	v_r3.u64 = REX_LOAD_U64(r27.u32 + 0);
	// bl 0x8205d740
	ctx.lr = 0x8207CD3C;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// mr r6,r3
	v_r6.u64 = v_r3.u64;
	// mr r5,r23
	v_r5.u64 = r23.u64;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CD50;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// mr r29,r3
	r29.u64 = v_r3.u64;
	// cmplwi cr6,r29,0
	cr6.compare<uint32_t>(r29.u32, 0, xer);
	// beq cr6,0x8207cd60
	if (cr6.eq) goto loc_8207CD60;
	// stfs f21,36(r29)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f21.f64);
	REX_STORE_U32(r29.u32 + 36, temp.u32);
loc_8207CD60:
	// lwz r11,24(r27)
	r11.u64 = REX_LOAD_U32(r27.u32 + 24);
	// extsw r3,r11
	v_r3.s64 = r11.s32;
	// bl 0x8205d740
	ctx.lr = 0x8207CD6C;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// mr r6,r3
	v_r6.u64 = v_r3.u64;
	// mr r5,r22
	v_r5.u64 = r22.u64;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CD80;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// mr r30,r3
	r30.u64 = v_r3.u64;
	// cmplwi cr6,r30,0
	cr6.compare<uint32_t>(r30.u32, 0, xer);
	// beq cr6,0x8207cd90
	if (cr6.eq) goto loc_8207CD90;
	// stfs f22,36(r30)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f22.f64);
	REX_STORE_U32(r30.u32 + 36, temp.u32);
loc_8207CD90:
	// addi r5,r1,112
	v_r5.s64 = ctx.r1.s64 + 112;
	// addi r4,r1,240
	v_r4.s64 = ctx.r1.s64 + 240;
	// mr r3,r28
	v_r3.u64 = r28.u64;
	// bl 0x8207c790
	ctx.lr = 0x8207CDA0;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5;
	sub_8207C790(ctx, base);
	// cmplwi cr6,r20,0
	cr6.compare<uint32_t>(r20.u32, 0, xer);
	// beq cr6,0x8207cdc8
	if (cr6.eq) goto loc_8207CDC8;
	// addi r4,r1,112
	v_r4.s64 = ctx.r1.s64 + 112;
	// lwz r6,8(r27)
	v_r6.u64 = REX_LOAD_U32(r27.u32 + 8);
	// mr r5,r24
	v_r5.u64 = r24.u64;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// bl 0x82076e40
	ctx.lr = 0x8207CDBC;
	ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6;
	sub_82076E40(ctx, base);
	v_r3 = ctx.r3;
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(v_r3.u32, 0, xer);
	// beq cr6,0x8207cdc8
	if (cr6.eq) goto loc_8207CDC8;
	// stfs f20,24(r3)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f20.f64);
	REX_STORE_U32(v_r3.u32 + 24, temp.u32);
loc_8207CDC8:
	// lfs f0,28(r29)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r29.u32 + 28);
	f0.f64 = double(temp.f32);
	// fadds f0,f0,f19
	f0.f64 = double(float(f0.f64 + f19.f64));
	// stfs f0,28(r29)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r29.u32 + 28, temp.u32);
	// lfs f0,28(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 28);
	f0.f64 = double(temp.f32);
	// stw r16,32(r30)
	REX_STORE_U32(r30.u32 + 32, r16.u32);
	// fadds f0,f0,f19
	f0.f64 = double(float(f0.f64 + f19.f64));
	// stfs f0,28(r30)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(r30.u32 + 28, temp.u32);
	// stw r15,32(r29)
	REX_STORE_U32(r29.u32 + 32, r15.u32);
loc_8207CDE8:
	// mr r8,r25
	v_r8.u64 = r25.u64;
	// fmr f5,f25
	ctx.fpscr.disableFlushMode();
	v_f5.f64 = f25.f64;
	// addi r3,r1,240
	v_r3.s64 = ctx.r1.s64 + 240;
	// fmr f4,f26
	v_f4.f64 = f26.f64;
	// fmr f3,f27
	v_f3.f64 = f27.f64;
	// fmr f2,f29
	v_f2.f64 = f29.f64;
	// fmr f1,f24
	v_f1.f64 = f24.f64;
	// bl 0x820770e8
	ctx.lr = 0x8207CE08;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.r3 = v_r3; ctx.r8 = v_r8;
	sub_820770E8(ctx, base);
	v_f13 = ctx.f13; v_r5 = ctx.r5; v_r6 = ctx.r6; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// lwz r11,15348(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15348);
	// addi r26,r26,1
	r26.s64 = r26.s64 + 1;
	// addi r27,r27,120
	r27.s64 = r27.s64 + 120;
	// cmpw cr6,r26,r11
	cr6.compare<int32_t>(r26.s32, r11.s32, xer);
	// blt cr6,0x8207cb84
	if (cr6.lt) goto loc_8207CB84;
loc_8207CE1C:
	// lwz r11,15836(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15836);
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// cmpwi cr6,r11,2
	cr6.compare<int32_t>(r11.s32, 2, xer);
	// lis r11,-32225
	r11.s64 = -2111897600;
	// lfs f30,4428(r9)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r9.u32 + 4428);
	f30.f64 = double(temp.f32);
	// addi r28,r11,17936
	r28.s64 = r11.s64 + 17936;
	// lfs f31,3952(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3952);
	f31.f64 = double(temp.f32);
	// bne cr6,0x8207cf04
	if (!cr6.eq) goto loc_8207CF04;
	// addi r11,r1,96
	r11.s64 = ctx.r1.s64 + 96;
	// lfs f0,14840(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 14840);
	f0.f64 = double(temp.f32);
	// fctiwz f13,f0
	v_f13.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// lfs f0,15848(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 15848);
	f0.f64 = double(temp.f32);
	// fctiwz f0,f0
	f0.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// stfiwx f13,0,r11
	REX_STORE_U32(r11.u32, v_f13.u32);
	// lwz r11,96(r1)
	r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// addi r10,r1,96
	v_r10.s64 = ctx.r1.s64 + 96;
	// stfiwx f0,0,r10
	REX_STORE_U32(v_r10.u32, f0.u32);
	// lwz r10,96(r1)
	v_r10.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// subf r11,r11,r10
	r11.u64 = v_r10.u64 - r11.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bge cr6,0x8207ce7c
	if (!cr6.lt) goto loc_8207CE7C;
	// mr r11,r15
	r11.u64 = r15.u64;
	// b 0x8207ce88
	goto loc_8207CE88;
loc_8207CE7C:
	// cmpwi cr6,r11,240
	cr6.compare<int32_t>(r11.s32, 240, xer);
	// ble cr6,0x8207ce88
	if (!cr6.gt) goto loc_8207CE88;
	// li r11,240
	r11.s64 = 240;
loc_8207CE88:
	// lis r10,-30584
	v_r10.s64 = -2004353024;
	// lfs f4,8(r17)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r17.u32 + 8);
	v_f4.f64 = double(temp.f32);
	// li r8,60
	v_r8.s64 = 60;
	// fmr f5,f30
	v_f5.f64 = f30.f64;
	// ori r30,r10,34953
	r30.u64 = v_r10.u64 | 34953;
	// fmr f3,f19
	v_f3.f64 = f19.f64;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// fmr f2,f18
	v_f2.f64 = f18.f64;
	// mulhw r30,r11,r30
	r30.s64 = (int64_t(r11.s32) * int64_t(r30.s32)) >> 32;
	// fmr f1,f31
	v_f1.f64 = f31.f64;
	// stfs f5,52(r1)
	temp.f32 = float(v_f5.f64);
	REX_STORE_U32(ctx.r1.u32 + 52, temp.u32);
	// ld r7,52(r1)
	v_r7.u64 = REX_LOAD_U64(ctx.r1.u32 + 52);
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// add r30,r30,r11
	r30.u64 = r30.u64 + r11.u64;
	// addi r9,r10,18884
	v_r9.s64 = v_r10.s64 + 18884;
	// divw r10,r11,r8
	v_r10.u64 = uint32_t((v_r8.s32 && !(r11.s32 == INT32_MIN && v_r8.s32 == -1)) ? r11.s32 / v_r8.s32 : 0);
	// srawi r30,r30,5
	xer.ca = (r30.s32 < 0) & ((r30.u32 & 0x1F) != 0);
	r30.s64 = r30.s32 >> 5;
	// addi r8,r28,32
	v_r8.s64 = r28.s64 + 32;
	// rlwinm r29,r30,1,31,31
	r29.u64 = __builtin_rotateleft64(r30.u32 | (r30.u64 << 32), 1) & 0x1;
	// add r30,r30,r29
	r30.u64 = r30.u64 + r29.u64;
	// mulli r30,r30,60
	r30.s64 = static_cast<int64_t>(r30.u64 * static_cast<uint64_t>(60));
	// subf r11,r30,r11
	r11.u64 = r11.u64 - r30.u64;
	// stw r11,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, r11.u32);
	// bl 0x820765a8
	ctx.lr = 0x8207CF04;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820765A8(ctx, base);
	v_f13 = ctx.f13; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_8207CF04:
	// lwz r11,15844(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15844);
	// cmpwi cr6,r11,2
	cr6.compare<int32_t>(r11.s32, 2, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// lfs f0,15852(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 15852);
	f0.f64 = double(temp.f32);
	// lis r11,20971
	r11.s64 = 1374355456;
	// lfs f13,14844(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 14844);
	v_f13.f64 = double(temp.f32);
	// addi r10,r1,96
	v_r10.s64 = ctx.r1.s64 + 96;
	// fsubs f13,f0,f13
	v_f13.f64 = double(float(f0.f64 - v_f13.f64));
	// ori r9,r11,34079
	v_r9.u64 = r11.u64 | 34079;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfd f12,3376(r11)
	v_f12.u64 = REX_LOAD_U64(r11.u32 + 3376);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fneg f11,f13
	v_f11.u64 = v_f13.u64 ^ 0x8000000000000000;
	// fsubs f10,f13,f0
	v_f10.f64 = double(float(v_f13.f64 - f0.f64));
	// fsel f13,f11,f12,f13
	v_f13.f64 = v_f11.f64 >= 0.0 ? v_f12.f64 : v_f13.f64;
	// fsel f0,f10,f0,f13
	f0.f64 = v_f10.f64 >= 0.0 ? f0.f64 : v_f13.f64;
	// lfs f13,4128(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 4128);
	v_f13.f64 = double(temp.f32);
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// fmuls f13,f0,f13
	v_f13.f64 = double(float(f0.f64 * v_f13.f64));
	// fctiwz f0,f0
	f0.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// stfiwx f0,0,r10
	REX_STORE_U32(v_r10.u32, f0.u32);
	// addi r11,r1,96
	r11.s64 = ctx.r1.s64 + 96;
	// lwz r10,96(r1)
	v_r10.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// cmpwi cr6,r10,10
	cr6.compare<int32_t>(v_r10.s32, 10, xer);
	// fctiwz f0,f13
	f0.s64 = __builtin_isnan(v_f13.f64) ? int64_t(0x80000000U) : (v_f13.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(v_f13.f64);
	// stfiwx f0,0,r11
	REX_STORE_U32(r11.u32, f0.u32);
	// lwz r11,96(r1)
	r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// mulhw r9,r11,r9
	v_r9.s64 = (int64_t(r11.s32) * int64_t(v_r9.s32)) >> 32;
	// srawi r9,r9,5
	xer.ca = (v_r9.s32 < 0) & ((v_r9.u32 & 0x1F) != 0);
	v_r9.s64 = v_r9.s32 >> 5;
	// rlwinm r8,r9,1,31,31
	v_r8.u64 = __builtin_rotateleft64(v_r9.u32 | (v_r9.u64 << 32), 1) & 0x1;
	// add r9,r9,r8
	v_r9.u64 = v_r9.u64 + v_r8.u64;
	// mulli r9,r9,100
	v_r9.s64 = static_cast<int64_t>(v_r9.u64 * static_cast<uint64_t>(100));
	// subf r31,r9,r11
	r31.u64 = r11.u64 - v_r9.u64;
	// bge cr6,0x8207cfc4
	if (!cr6.lt) goto loc_8207CFC4;
	// lfs f0,16(r28)
	temp.u32 = REX_LOAD_U32(r28.u32 + 16);
	f0.f64 = double(temp.f32);
	// addi r8,r1,192
	v_r8.s64 = ctx.r1.s64 + 192;
	// fmuls f0,f0,f20
	f0.f64 = double(float(f0.f64 * f20.f64));
	// stfs f0,192(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 192, temp.u32);
	// lfs f0,20(r28)
	temp.u32 = REX_LOAD_U32(r28.u32 + 20);
	f0.f64 = double(temp.f32);
	// fmuls f0,f0,f20
	f0.f64 = double(float(f0.f64 * f20.f64));
	// stfs f0,196(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 196, temp.u32);
	// lfs f0,24(r28)
	temp.u32 = REX_LOAD_U32(r28.u32 + 24);
	f0.f64 = double(temp.f32);
	// fmuls f0,f0,f20
	f0.f64 = double(float(f0.f64 * f20.f64));
	// stfs f0,200(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 200, temp.u32);
	// lfs f0,28(r28)
	temp.u32 = REX_LOAD_U32(r28.u32 + 28);
	f0.f64 = double(temp.f32);
	// fmuls f0,f0,f20
	f0.f64 = double(float(f0.f64 * f20.f64));
	// stfs f0,204(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 204, temp.u32);
	// b 0x8207cfc8
	goto loc_8207CFC8;
loc_8207CFC4:
	// addi r8,r28,32
	v_r8.s64 = r28.s64 + 32;
loc_8207CFC8:
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f4,8(r17)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r17.u32 + 8);
	v_f4.f64 = double(temp.f32);
	// fmr f5,f30
	v_f5.f64 = f30.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// fmr f3,f19
	v_f3.f64 = f19.f64;
	// addi r9,r11,18872
	v_r9.s64 = r11.s64 + 18872;
	// fmr f2,f18
	v_f2.f64 = f18.f64;
	// stfs f5,52(r1)
	temp.f32 = float(v_f5.f64);
	REX_STORE_U32(ctx.r1.u32 + 52, temp.u32);
	// fmr f1,f31
	v_f1.f64 = f31.f64;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r7,52(r1)
	v_r7.u64 = REX_LOAD_U64(ctx.r1.u32 + 52);
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// stw r31,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, r31.u32);
	// bl 0x820765a8
	ctx.lr = 0x8207D014;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f10 = v_f10; ctx.f11 = v_f11; ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820765A8(ctx, base);
	// addi r1,r1,1984
	ctx.r1.s64 = ctx.r1.s64 + 1984;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e8bc
	ctx.lr = 0x8207D020;
	// b 0x8212e954
	return;
loc_8207D024:
	// lwz r11,15844(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15844);
	// lis r6,-32256
	v_r6.s64 = -2113929216;
	// lis r7,-32256
	v_r7.s64 = -2113929216;
	// cmpwi cr6,r11,2
	cr6.compare<int32_t>(r11.s32, 2, xer);
	// lis r8,-32256
	v_r8.s64 = -2113929216;
	// lis r9,-32256
	v_r9.s64 = -2113929216;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// lfs f28,7284(r6)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r6.u32 + 7284);
	f28.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f25,8364(r7)
	temp.u32 = REX_LOAD_U32(v_r7.u32 + 8364);
	f25.f64 = double(temp.f32);
	// lfs f22,4128(r8)
	temp.u32 = REX_LOAD_U32(v_r8.u32 + 4128);
	f22.f64 = double(temp.f32);
	// lfd f23,3376(r9)
	f23.u64 = REX_LOAD_U64(v_r9.u32 + 3376);
	// lfs f26,3432(r10)
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3432);
	f26.f64 = double(temp.f32);
	// lfs f24,1732(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 1732);
	f24.f64 = double(temp.f32);
	// bne cr6,0x8207d238
	if (!cr6.eq) goto loc_8207D238;
	// lfs f0,15852(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 15852);
	f0.f64 = double(temp.f32);
	// addi r11,r1,96
	r11.s64 = ctx.r1.s64 + 96;
	// lfs f13,14844(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 14844);
	v_f13.f64 = double(temp.f32);
	// lis r10,20971
	v_r10.s64 = 1374355456;
	// fsubs f13,f0,f13
	v_f13.f64 = double(float(f0.f64 - v_f13.f64));
	// ori r9,r10,34079
	v_r9.u64 = v_r10.u64 | 34079;
	// fneg f12,f13
	v_f12.u64 = v_f13.u64 ^ 0x8000000000000000;
	// fsubs f11,f13,f0
	v_f11.f64 = double(float(v_f13.f64 - f0.f64));
	// fsel f13,f12,f23,f13
	v_f13.f64 = v_f12.f64 >= 0.0 ? f23.f64 : v_f13.f64;
	// fsel f0,f11,f0,f13
	f0.f64 = v_f11.f64 >= 0.0 ? f0.f64 : v_f13.f64;
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// fmuls f13,f0,f22
	v_f13.f64 = double(float(f0.f64 * f22.f64));
	// fctiwz f0,f0
	f0.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// stfiwx f0,0,r11
	REX_STORE_U32(r11.u32, f0.u32);
	// addi r11,r1,96
	r11.s64 = ctx.r1.s64 + 96;
	// lwz r10,96(r1)
	v_r10.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// cmpwi cr6,r10,10
	cr6.compare<int32_t>(v_r10.s32, 10, xer);
	// fctiwz f0,f13
	f0.s64 = __builtin_isnan(v_f13.f64) ? int64_t(0x80000000U) : (v_f13.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(v_f13.f64);
	// stfiwx f0,0,r11
	REX_STORE_U32(r11.u32, f0.u32);
	// lwz r11,96(r1)
	r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// mulhw r9,r11,r9
	v_r9.s64 = (int64_t(r11.s32) * int64_t(v_r9.s32)) >> 32;
	// srawi r9,r9,5
	xer.ca = (v_r9.s32 < 0) & ((v_r9.u32 & 0x1F) != 0);
	v_r9.s64 = v_r9.s32 >> 5;
	// rlwinm r8,r9,1,31,31
	v_r8.u64 = __builtin_rotateleft64(v_r9.u32 | (v_r9.u64 << 32), 1) & 0x1;
	// add r9,r9,r8
	v_r9.u64 = v_r9.u64 + v_r8.u64;
	// mulli r9,r9,100
	v_r9.s64 = static_cast<int64_t>(v_r9.u64 * static_cast<uint64_t>(100));
	// subf r29,r9,r11
	r29.u64 = r11.u64 - v_r9.u64;
	// lis r11,-32225
	r11.s64 = -2111897600;
	// addi r30,r11,17936
	r30.s64 = r11.s64 + 17936;
	// bge cr6,0x8207d114
	if (!cr6.lt) goto loc_8207D114;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f13,16(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 16);
	v_f13.f64 = double(temp.f32);
	// addi r8,r1,160
	v_r8.s64 = ctx.r1.s64 + 160;
	// lfs f0,1788(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 1788);
	f0.f64 = double(temp.f32);
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,160(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 160, temp.u32);
	// lfs f13,20(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 20);
	v_f13.f64 = double(temp.f32);
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,164(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 164, temp.u32);
	// lfs f13,24(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 24);
	v_f13.f64 = double(temp.f32);
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,168(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 168, temp.u32);
	// lfs f13,28(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 28);
	v_f13.f64 = double(temp.f32);
	// fmuls f0,f13,f0
	f0.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f0,172(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 172, temp.u32);
	// b 0x8207d118
	goto loc_8207D118;
loc_8207D114:
	// addi r8,r30,32
	v_r8.s64 = r30.s64 + 32;
loc_8207D118:
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f3,f26
	ctx.fpscr.disableFlushMode();
	v_f3.f64 = f26.f64;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// addi r9,r11,18872
	v_r9.s64 = r11.s64 + 18872;
	// stw r29,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, r29.u32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f5,4428(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 4428);
	v_f5.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f5,52(r1)
	temp.f32 = float(v_f5.f64);
	REX_STORE_U32(ctx.r1.u32 + 52, temp.u32);
	// ld r7,52(r1)
	v_r7.u64 = REX_LOAD_U64(ctx.r1.u32 + 52);
	// addi r11,r11,18740
	r11.s64 = r11.s64 + 18740;
	// lfs f4,8(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 8);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// addi r11,r11,18660
	r11.s64 = r11.s64 + 18660;
	// lfs f31,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f31.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// addi r11,r11,3952
	r11.s64 = r11.s64 + 3952;
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// lfs f30,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f30.f64 = double(temp.f32);
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820765a8
	ctx.lr = 0x8207D188;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f11 = v_f11; ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820765A8(ctx, base);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f3,15440(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 15440);
	v_f3.f64 = double(temp.f32);
	// li r10,0
	v_r10.s64 = 0;
	// lwz r8,15400(r31)
	v_r8.u64 = REX_LOAD_U32(r31.u32 + 15400);
	// li r9,12
	v_r9.s64 = 12;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// li r6,2
	v_r6.s64 = 2;
	// lfs f29,18868(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18868);
	f29.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f4,f29
	v_f4.f64 = f29.f64;
	// lfs f1,18864(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18864);
	v_f1.f64 = double(temp.f32);
	// bl 0x8207c530
	ctx.lr = 0x8207D1B8;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r6 = v_r6; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8207C530(ctx, base);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lwz r8,15404(r31)
	v_r8.u64 = REX_LOAD_U32(r31.u32 + 15404);
	// li r10,0
	v_r10.s64 = 0;
	// lfs f3,15444(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 15444);
	v_f3.f64 = double(temp.f32);
	// li r9,2
	v_r9.s64 = 2;
	// fmr f4,f29
	v_f4.f64 = f29.f64;
	// li r6,2
	v_r6.s64 = 2;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// lfs f1,18860(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18860);
	v_f1.f64 = double(temp.f32);
	// bl 0x8207c530
	ctx.lr = 0x8207D1E0;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r6 = v_r6; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8207C530(ctx, base);
	// lwz r11,15416(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15416);
	// extsw r3,r11
	v_r3.s64 = r11.s32;
	// bl 0x8205d740
	ctx.lr = 0x8207D1EC;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f4,f28
	ctx.fpscr.disableFlushMode();
	v_f4.f64 = f28.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// addi r8,r11,7528
	v_r8.s64 = r11.s64 + 7528;
	// fmr f3,f28
	v_f3.f64 = f28.f64;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f1,f25
	v_f1.f64 = f25.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// lfs f2,4160(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 4160);
	v_f2.f64 = double(temp.f32);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// bl 0x82076130
	ctx.lr = 0x8207D234;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9;
	sub_82076130(ctx, base);
	// b 0x8207d554
	goto loc_8207D554;
loc_8207D238:
	// lwz r11,15836(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15836);
	// cmpwi cr6,r11,2
	cr6.compare<int32_t>(r11.s32, 2, xer);
	// bne cr6,0x8207d408
	if (!cr6.eq) goto loc_8207D408;
	// addi r11,r1,96
	r11.s64 = ctx.r1.s64 + 96;
	// lfs f0,15848(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 15848);
	f0.f64 = double(temp.f32);
	// fctiwz f13,f0
	v_f13.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// lfs f0,14840(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 14840);
	f0.f64 = double(temp.f32);
	// fctiwz f0,f0
	f0.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// stfiwx f13,0,r11
	REX_STORE_U32(r11.u32, v_f13.u32);
	// lwz r11,96(r1)
	r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// addi r10,r1,96
	v_r10.s64 = ctx.r1.s64 + 96;
	// stfiwx f0,0,r10
	REX_STORE_U32(v_r10.u32, f0.u32);
	// lwz r10,96(r1)
	v_r10.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// subf r29,r10,r11
	r29.u64 = r11.u64 - v_r10.u64;
	// cmpwi cr6,r29,0
	cr6.compare<int32_t>(r29.s32, 0, xer);
	// bge cr6,0x8207d280
	if (!cr6.lt) goto loc_8207D280;
	// mr r29,r15
	r29.u64 = r15.u64;
	// b 0x8207d294
	goto loc_8207D294;
loc_8207D280:
	// cmpw cr6,r29,r11
	cr6.compare<int32_t>(r29.s32, r11.s32, xer);
	// ble cr6,0x8207d28c
	if (!cr6.gt) goto loc_8207D28C;
	// mr r29,r11
	r29.u64 = r11.u64;
loc_8207D28C:
	// cmpwi cr6,r29,10
	cr6.compare<int32_t>(r29.s32, 10, xer);
	// bge cr6,0x8207d2dc
	if (!cr6.lt) goto loc_8207D2DC;
loc_8207D294:
	// lis r11,-32225
	r11.s64 = -2111897600;
	// addi r8,r1,176
	v_r8.s64 = ctx.r1.s64 + 176;
	// addi r30,r11,17936
	r30.s64 = r11.s64 + 17936;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f13,16(r30)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r30.u32 + 16);
	v_f13.f64 = double(temp.f32);
	// lfs f0,1788(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 1788);
	f0.f64 = double(temp.f32);
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,176(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 176, temp.u32);
	// lfs f13,20(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 20);
	v_f13.f64 = double(temp.f32);
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,180(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 180, temp.u32);
	// lfs f13,24(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 24);
	v_f13.f64 = double(temp.f32);
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,184(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 184, temp.u32);
	// lfs f13,28(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 28);
	v_f13.f64 = double(temp.f32);
	// fmuls f0,f13,f0
	f0.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f0,188(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 188, temp.u32);
	// b 0x8207d2e8
	goto loc_8207D2E8;
loc_8207D2DC:
	// lis r11,-32225
	r11.s64 = -2111897600;
	// addi r30,r11,17936
	r30.s64 = r11.s64 + 17936;
	// addi r8,r30,32
	v_r8.s64 = r30.s64 + 32;
loc_8207D2E8:
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f3,f26
	ctx.fpscr.disableFlushMode();
	v_f3.f64 = f26.f64;
	// li r10,60
	v_r10.s64 = 60;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// divw r10,r29,r10
	v_r10.u64 = uint32_t((v_r10.s32 && !(r29.s32 == INT32_MIN && v_r10.s32 == -1)) ? r29.s32 / v_r10.s32 : 0);
	// lfs f5,4428(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 4428);
	v_f5.f64 = double(temp.f32);
	// lis r11,-30584
	r11.s64 = -2004353024;
	// stfs f5,52(r1)
	temp.f32 = float(v_f5.f64);
	REX_STORE_U32(ctx.r1.u32 + 52, temp.u32);
	// ld r7,52(r1)
	v_r7.u64 = REX_LOAD_U64(ctx.r1.u32 + 52);
	// ori r28,r11,34953
	r28.u64 = r11.u64 | 34953;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// addi r11,r11,18740
	r11.s64 = r11.s64 + 18740;
	// lfs f4,8(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 8);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// addi r9,r11,18884
	v_r9.s64 = r11.s64 + 18884;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// addi r11,r11,18660
	r11.s64 = r11.s64 + 18660;
	// lfs f31,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f31.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// addi r11,r11,3952
	r11.s64 = r11.s64 + 3952;
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// lfs f30,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f30.f64 = double(temp.f32);
	// mulhw r11,r29,r28
	r11.s64 = (int64_t(r29.s32) * int64_t(r28.s32)) >> 32;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// add r11,r11,r29
	r11.u64 = r11.u64 + r29.u64;
	// srawi r11,r11,5
	xer.ca = (r11.s32 < 0) & ((r11.u32 & 0x1F) != 0);
	r11.s64 = r11.s32 >> 5;
	// rlwinm r28,r11,1,31,31
	r28.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 1) & 0x1;
	// add r11,r11,r28
	r11.u64 = r11.u64 + r28.u64;
	// mulli r11,r11,60
	r11.s64 = static_cast<int64_t>(r11.u64 * static_cast<uint64_t>(60));
	// subf r11,r11,r29
	r11.u64 = r29.u64 - r11.u64;
	// stw r11,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, r11.u32);
	// bl 0x820765a8
	ctx.lr = 0x8207D384;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820765A8(ctx, base);
	// lwz r11,15416(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15416);
	// extsw r3,r11
	v_r3.s64 = r11.s32;
	// bl 0x8205d740
	ctx.lr = 0x8207D390;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f4,f28
	ctx.fpscr.disableFlushMode();
	v_f4.f64 = f28.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// addi r8,r11,7528
	v_r8.s64 = r11.s64 + 7528;
	// fmr f3,f28
	v_f3.f64 = f28.f64;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// lfs f1,18864(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18864);
	v_f1.f64 = double(temp.f32);
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820764c0
	ctx.lr = 0x8207D3D8;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9;
	sub_820764C0(ctx, base);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// li r10,0
	v_r10.s64 = 0;
	// lfs f3,15444(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 15444);
	v_f3.f64 = double(temp.f32);
	// li r9,2
	v_r9.s64 = 2;
	// lwz r8,15404(r31)
	v_r8.u64 = REX_LOAD_U32(r31.u32 + 15404);
	// li r6,2
	v_r6.s64 = 2;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// lfs f4,18868(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18868);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f1,18860(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18860);
	v_f1.f64 = double(temp.f32);
	// bl 0x8207c530
	ctx.lr = 0x8207D404;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r6 = v_r6; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8207C530(ctx, base);
	// b 0x8207d554
	goto loc_8207D554;
loc_8207D408:
	// lwz r11,15860(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15860);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x8207d48c
	if (!cr6.eq) goto loc_8207D48C;
	// lwz r11,15416(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15416);
	// extsw r3,r11
	v_r3.s64 = r11.s32;
	// bl 0x8205d740
	ctx.lr = 0x8207D420;
	ctx.r3 = v_r3; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32225
	r11.s64 = -2111897600;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// addi r30,r11,17936
	r30.s64 = r11.s64 + 17936;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// addi r8,r11,7528
	v_r8.s64 = r11.s64 + 7528;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f4,6932(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 6932);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f3,f4
	v_f3.f64 = v_f4.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// addi r11,r11,18660
	r11.s64 = r11.s64 + 18660;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// lfs f31,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f31.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// addi r11,r11,3952
	r11.s64 = r11.s64 + 3952;
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// lfs f30,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f30.f64 = double(temp.f32);
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820764c0
	ctx.lr = 0x8207D488;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9;
	sub_820764C0(ctx, base);
	// b 0x8207d554
	goto loc_8207D554;
loc_8207D48C:
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f3,15440(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 15440);
	v_f3.f64 = double(temp.f32);
	// li r10,0
	v_r10.s64 = 0;
	// lwz r8,15400(r31)
	v_r8.u64 = REX_LOAD_U32(r31.u32 + 15400);
	// addi r11,r11,18660
	r11.s64 = r11.s64 + 18660;
	// fmr f4,f24
	v_f4.f64 = f24.f64;
	// li r9,12
	v_r9.s64 = 12;
	// li r6,1
	v_r6.s64 = 1;
	// lfs f31,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f31.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// lfs f1,18856(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18856);
	v_f1.f64 = double(temp.f32);
	// bl 0x8207c530
	ctx.lr = 0x8207D4C0;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8207C530(ctx, base);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lwz r8,15404(r31)
	v_r8.u64 = REX_LOAD_U32(r31.u32 + 15404);
	// li r10,0
	v_r10.s64 = 0;
	// lfs f3,15444(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 15444);
	v_f3.f64 = double(temp.f32);
	// li r9,2
	v_r9.s64 = 2;
	// fmr f4,f24
	v_f4.f64 = f24.f64;
	// li r6,0
	v_r6.s64 = 0;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// lfs f1,18852(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18852);
	v_f1.f64 = double(temp.f32);
	// bl 0x8207c530
	ctx.lr = 0x8207D4E8;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r6 = v_r6; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_8207C530(ctx, base);
	// lwz r11,15416(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15416);
	// extsw r3,r11
	v_r3.s64 = r11.s32;
	// bl 0x8205d740
	ctx.lr = 0x8207D4F4;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32225
	r11.s64 = -2111897600;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// addi r30,r11,17936
	r30.s64 = r11.s64 + 17936;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// addi r8,r11,7528
	v_r8.s64 = r11.s64 + 7528;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f4,6932(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 6932);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f3,f4
	v_f3.f64 = v_f4.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// lfs f2,18848(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18848);
	v_f2.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// addi r11,r11,3952
	r11.s64 = r11.s64 + 3952;
	// lfs f30,0(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 0);
	f30.f64 = double(temp.f32);
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820764c0
	ctx.lr = 0x8207D554;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9;
	sub_820764C0(ctx, base);
loc_8207D554:
	// li r3,122
	v_r3.s64 = 122;
	// bl 0x820826d8
	ctx.lr = 0x8207D55C;
	ctx.r3 = v_r3;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// mr r8,r3
	v_r8.u64 = v_r3.u64;
	// fmr f4,f28
	ctx.fpscr.disableFlushMode();
	v_f4.f64 = f28.f64;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// fmr f1,f25
	v_f1.f64 = f25.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// lfs f29,4596(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 4596);
	f29.f64 = double(temp.f32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// fmr f3,f29
	v_f3.f64 = f29.f64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// bl 0x82076130
	ctx.lr = 0x8207D5A0;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8;
	sub_82076130(ctx, base);
	// ld r3,15392(r31)
	v_r3.u64 = REX_LOAD_U64(r31.u32 + 15392);
	// bl 0x8205d740
	ctx.lr = 0x8207D5A8;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f4,f26
	ctx.fpscr.disableFlushMode();
	v_f4.f64 = f26.f64;
	// fmr f3,f29
	v_f3.f64 = f29.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// addi r25,r11,3284
	r25.s64 = r11.s64 + 3284;
	// fmr f1,f25
	v_f1.f64 = f25.f64;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// mr r8,r25
	v_r8.u64 = r25.u64;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// lfs f27,3992(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3992);
	f27.f64 = double(temp.f32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// fmr f2,f27
	v_f2.f64 = f27.f64;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// bl 0x82076130
	ctx.lr = 0x8207D5F8;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9;
	sub_82076130(ctx, base);
	// li r3,123
	v_r3.s64 = 123;
	// bl 0x820826d8
	ctx.lr = 0x8207D600;
	ctx.r3 = v_r3;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// lwz r11,15844(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15844);
	// mr r26,r3
	r26.u64 = v_r3.u64;
	// mulli r10,r11,880
	v_r10.s64 = static_cast<int64_t>(r11.u64 * static_cast<uint64_t>(880));
	// mr r3,r11
	v_r3.u64 = r11.u64;
	// addi r11,r31,88
	r11.s64 = r31.s64 + 88;
	// ldx r27,r10,r11
	r27.u64 = REX_LOAD_U64(v_r10.u32 + r11.u32);
	// bl 0x8208bcf8
	ctx.lr = 0x8207D61C;
	ctx.r3 = v_r3; ctx.r10 = v_r10;
	sub_8208BCF8(ctx, base);
	v_r3 = ctx.r3;
	// lbz r11,15877(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 15877);
	// mr r29,r3
	r29.u64 = v_r3.u64;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x8207d650
	if (cr6.eq) goto loc_8207D650;
	// lwz r11,15844(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15844);
	// addi r10,r31,7128
	v_r10.s64 = r31.s64 + 7128;
	// mulli r11,r11,880
	r11.s64 = static_cast<int64_t>(r11.u64 * static_cast<uint64_t>(880));
	// ldx r27,r11,r10
	r27.u64 = REX_LOAD_U64(r11.u32 + v_r10.u32);
	// ld r11,15392(r31)
	r11.u64 = REX_LOAD_U64(r31.u32 + 15392);
	// cmpd cr6,r11,r27
	cr6.compare<int64_t>(r11.s64, r27.s64, xer);
	// ble cr6,0x8207d764
	if (!cr6.gt) goto loc_8207D764;
	// mr r27,r11
	r27.u64 = r11.u64;
	// b 0x8207d764
	goto loc_8207D764;
loc_8207D650:
	// ld r11,15392(r31)
	r11.u64 = REX_LOAD_U64(r31.u32 + 15392);
	// cmpd cr6,r11,r27
	cr6.compare<int64_t>(r11.s64, r27.s64, xer);
	// ble cr6,0x8207d660
	if (!cr6.gt) goto loc_8207D660;
	// mr r27,r11
	r27.u64 = r11.u64;
loc_8207D660:
	// bl 0x82033600
	ctx.lr = 0x8207D664;
	sub_82033600(ctx, base);
	// bl 0x8207e248
	ctx.lr = 0x8207D668;
	sub_8207E248(ctx, base);
	v_r3 = ctx.r3; v_r10 = ctx.r10;
	// lwz r11,8(r3)
	r11.u64 = REX_LOAD_U32(v_r3.u32 + 8);
	// cmpwi cr6,r11,2
	cr6.compare<int32_t>(r11.s32, 2, xer);
	// bne cr6,0x8207d764
	if (!cr6.eq) goto loc_8207D764;
	// lis r11,-32225
	r11.s64 = -2111897600;
	// mr r4,r29
	v_r4.u64 = r29.u64;
	// addi r28,r11,-16776
	r28.s64 = r11.s64 + -16776;
	// mr r3,r28
	v_r3.u64 = r28.u64;
	// bl 0x8208bbf8
	ctx.lr = 0x8207D688;
	ctx.r3 = v_r3; ctx.r4 = v_r4;
	sub_8208BBF8(ctx, base);
	v_r3 = ctx.r3; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// beq cr6,0x8207d764
	if (cr6.eq) goto loc_8207D764;
	// mulli r11,r29,4072
	r11.s64 = static_cast<int64_t>(r29.u64 * static_cast<uint64_t>(4072));
	// addi r10,r28,8128
	v_r10.s64 = r28.s64 + 8128;
	// add r29,r11,r10
	r29.u64 = r11.u64 + v_r10.u64;
	// bl 0x82033600
	ctx.lr = 0x8207D6A4;
	ctx.r10 = v_r10;
	sub_82033600(ctx, base);
	// bl 0x82093600
	ctx.lr = 0x8207D6A8;
	sub_82093600(ctx, base);
	v_r3 = ctx.r3;
	// lwz r11,12(r3)
	r11.u64 = REX_LOAD_U32(v_r3.u32 + 12);
	// cmplwi cr6,r11,1
	cr6.compare<uint32_t>(r11.u32, 1, xer);
	// beq cr6,0x8207d754
	if (cr6.eq) goto loc_8207D754;
	// lwz r11,0(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + 0);
	// mr r10,r15
	v_r10.u64 = r15.u64;
	// addi r11,r11,-1
	r11.s64 = r11.s64 + -1;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// blt cr6,0x8207d6cc
	if (cr6.lt) goto loc_8207D6CC;
	// mr r10,r11
	v_r10.u64 = r11.u64;
loc_8207D6CC:
	// lwz r11,8(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + 8);
	// addi r11,r11,-1
	r11.s64 = r11.s64 + -1;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bge cr6,0x8207d6e4
	if (!cr6.lt) goto loc_8207D6E4;
	// mr r11,r15
	r11.u64 = r15.u64;
	// b 0x8207d6f0
	goto loc_8207D6F0;
loc_8207D6E4:
	// cmpw cr6,r11,r10
	cr6.compare<int32_t>(r11.s32, v_r10.s32, xer);
	// ble cr6,0x8207d6f0
	if (!cr6.gt) goto loc_8207D6F0;
	// mr r11,r10
	r11.u64 = v_r10.u64;
loc_8207D6F0:
	// mr r9,r11
	v_r9.u64 = r11.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// blt cr6,0x8207d764
	if (cr6.lt) goto loc_8207D764;
	// addi r11,r11,1
	r11.s64 = r11.s64 + 1;
	// ld r7,15392(r31)
	v_r7.u64 = REX_LOAD_U64(r31.u32 + 15392);
	// rlwinm r10,r11,2,0,29
	v_r10.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 2) & 0xFFFFFFFC;
	// add r11,r11,r10
	r11.u64 = r11.u64 + v_r10.u64;
	// rlwinm r11,r11,3,0,28
	r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 3) & 0xFFFFFFF8;
	// add r11,r11,r29
	r11.u64 = r11.u64 + r29.u64;
loc_8207D714:
	
	// addi r8,r11,-8
	v_r8.s64 = r11.s64 + -8;
	// cmpwi cr6,r9,0
	cr6.compare<int32_t>(v_r9.s32, 0, xer);
	// ble cr6,0x8207d748
	if (!cr6.gt) goto loc_8207D748;
	// ld r10,0(r11)
	v_r10.u64 = REX_LOAD_U64(r11.u32 + 0);
	// cmpdi cr6,r10,0
	cr6.compare<int64_t>(v_r10.s64, 0, xer);
	// ble cr6,0x8207d734
	if (!cr6.gt) goto loc_8207D734;
	// cmpd cr6,r7,r10
	cr6.compare<int64_t>(v_r7.s64, v_r10.s64, xer);
	// ble cr6,0x8207d748
	if (!cr6.gt) goto loc_8207D748;
loc_8207D734:
	// addi r9,r9,-1
	v_r9.s64 = v_r9.s64 + -1;
	// addi r11,r11,-40
	r11.s64 = r11.s64 + -40;
	// cmpwi cr6,r9,0
	cr6.compare<int32_t>(v_r9.s32, 0, xer);
	// bge cr6,0x8207d714
	if (!cr6.lt) goto loc_8207D714;
	// b 0x8207d764
	goto loc_8207D764;
loc_8207D748:
	// ld r27,8(r8)
	r27.u64 = REX_LOAD_U64(v_r8.u32 + 8);
	// addi r26,r8,20
	r26.s64 = v_r8.s64 + 20;
	// b 0x8207d764
	goto loc_8207D764;
loc_8207D754:
	// bl 0x82033600
	ctx.lr = 0x8207D758;
	sub_82033600(ctx, base);
	// bl 0x8207e248
	ctx.lr = 0x8207D75C;
	sub_8207E248(ctx, base);
	v_r3 = ctx.r3; v_r10 = ctx.r10;
	// ld r27,16(r29)
	r27.u64 = REX_LOAD_U64(r29.u32 + 16);
	// addi r26,r3,12
	r26.s64 = v_r3.s64 + 12;
loc_8207D764:
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f2,f31
	ctx.fpscr.disableFlushMode();
	v_f2.f64 = f31.f64;
	// fmr f4,f28
	v_f4.f64 = f28.f64;
	// mr r9,r26
	v_r9.u64 = r26.u64;
	// fmr f3,f29
	v_f3.f64 = f29.f64;
	// addi r8,r30,32
	v_r8.s64 = r30.s64 + 32;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// lfs f5,18844(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18844);
	v_f5.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f5,52(r1)
	temp.f32 = float(v_f5.f64);
	REX_STORE_U32(ctx.r1.u32 + 52, temp.u32);
	// ld r7,52(r1)
	v_r7.u64 = REX_LOAD_U64(ctx.r1.u32 + 52);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// lfs f31,18840(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 18840);
	f31.f64 = double(temp.f32);
	// fmr f1,f31
	v_f1.f64 = f31.f64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820763d0
	ctx.lr = 0x8207D7B8;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.f5 = v_f5; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820763D0(ctx, base);
	// mr r3,r27
	v_r3.u64 = r27.u64;
	// bl 0x8205d740
	ctx.lr = 0x8207D7C0;
	ctx.r3 = v_r3;
	sub_8205D740(ctx, base);
	v_r3 = ctx.r3;
	// fmr f4,f26
	ctx.fpscr.disableFlushMode();
	v_f4.f64 = f26.f64;
	// fmr f3,f29
	v_f3.f64 = f29.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f2,f27
	v_f2.f64 = f27.f64;
	// mr r8,r25
	v_r8.u64 = r25.u64;
	// fmr f1,f31
	v_f1.f64 = f31.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820762f0
	ctx.lr = 0x8207D800;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9;
	sub_820762F0(ctx, base);
	v_f13 = ctx.f13; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// lbz r11,15877(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 15877);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// bl 0x82034588
	ctx.lr = 0x8207D810;
	sub_82034588(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// bl 0x8207e238
	ctx.lr = 0x8207D820;
	sub_8207E238(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// clrlwi r11,r3,24
	r11.u64 = v_r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// bl 0x82033600
	ctx.lr = 0x8207D830;
	sub_82033600(ctx, base);
	// bl 0x8207e248
	ctx.lr = 0x8207D834;
	sub_8207E248(ctx, base);
	v_f13 = ctx.f13; v_r3 = ctx.r3; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// mr r26,r3
	r26.u64 = v_r3.u64;
	// lfs f31,18836(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 18836);
	f31.f64 = double(temp.f32);
	// lwz r11,15844(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 15844);
	// cmpwi cr6,r11,5
	cr6.compare<int32_t>(r11.s32, 5, xer);
	// bne cr6,0x8207d948
	if (!cr6.eq) goto loc_8207D948;
	// lbz r11,15448(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 15448);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// lbz r11,15449(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 15449);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// lwz r31,15452(r31)
	r31.u64 = REX_LOAD_U32(r31.u32 + 15452);
	// cmpwi cr6,r31,10
	cr6.compare<int32_t>(r31.s32, 10, xer);
	// blt cr6,0x8207dd1c
	if (cr6.lt) goto loc_8207DD1C;
	// cmpwi cr6,r31,30
	cr6.compare<int32_t>(r31.s32, 30, xer);
	// bge cr6,0x8207d8e4
	if (!cr6.lt) goto loc_8207D8E4;
	// li r3,163
	v_r3.s64 = 163;
	// bl 0x820826d8
	ctx.lr = 0x8207D880;
	ctx.r3 = v_r3;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// li r29,30
	r29.s64 = 30;
	// fmr f2,f31
	ctx.fpscr.disableFlushMode();
	v_f2.f64 = f31.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// mr r10,r31
	v_r10.u64 = r31.u64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// lfs f4,3352(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3352);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f3,f4
	v_f3.f64 = v_f4.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// addi r8,r11,18820
	v_r8.s64 = r11.s64 + 18820;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// stw r29,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, r29.u32);
	// bl 0x820764c0
	ctx.lr = 0x8207D8D4;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820764C0(ctx, base);
	// addi r1,r1,1984
	ctx.r1.s64 = ctx.r1.s64 + 1984;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e8bc
	ctx.lr = 0x8207D8E0;
	// b 0x8212e954
	return;
loc_8207D8E4:
	// li r3,164
	v_r3.s64 = 164;
	// bl 0x820826d8
	ctx.lr = 0x8207D8EC;
	ctx.r3 = v_r3;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f2,f31
	ctx.fpscr.disableFlushMode();
	v_f2.f64 = f31.f64;
	// addi r8,r11,18812
	v_r8.s64 = r11.s64 + 18812;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// mr r10,r31
	v_r10.u64 = r31.u64;
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// lfs f4,3352(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3352);
	v_f4.f64 = double(temp.f32);
	// fmr f3,f4
	v_f3.f64 = v_f4.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// bl 0x820764c0
	ctx.lr = 0x8207D938;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820764C0(ctx, base);
	// addi r1,r1,1984
	ctx.r1.s64 = ctx.r1.s64 + 1984;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e8bc
	ctx.lr = 0x8207D944;
	// b 0x8212e954
	return;
loc_8207D948:
	// cmpwi cr6,r11,3
	cr6.compare<int32_t>(r11.s32, 3, xer);
	// bne cr6,0x8207da4c
	if (!cr6.eq) goto loc_8207DA4C;
	// lbz r11,15448(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 15448);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// lbz r11,15449(r31)
	r11.u64 = REX_LOAD_U8(r31.u32 + 15449);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// lwz r31,14864(r31)
	r31.u64 = REX_LOAD_U32(r31.u32 + 14864);
	// cmpwi cr6,r31,3
	cr6.compare<int32_t>(r31.s32, 3, xer);
	// ble cr6,0x8207dd1c
	if (!cr6.gt) goto loc_8207DD1C;
	// cmpwi cr6,r31,8
	cr6.compare<int32_t>(r31.s32, 8, xer);
	// bge cr6,0x8207d9e8
	if (!cr6.lt) goto loc_8207D9E8;
	// li r3,165
	v_r3.s64 = 165;
	// bl 0x820826d8
	ctx.lr = 0x8207D984;
	ctx.r3 = v_r3;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// li r29,8
	r29.s64 = 8;
	// fmr f2,f31
	ctx.fpscr.disableFlushMode();
	v_f2.f64 = f31.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// mr r10,r31
	v_r10.u64 = r31.u64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// lfs f4,3352(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3352);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f3,f4
	v_f3.f64 = v_f4.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// addi r8,r11,18820
	v_r8.s64 = r11.s64 + 18820;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// stw r29,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, r29.u32);
	// bl 0x820764c0
	ctx.lr = 0x8207D9D8;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820764C0(ctx, base);
	// addi r1,r1,1984
	ctx.r1.s64 = ctx.r1.s64 + 1984;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e8bc
	ctx.lr = 0x8207D9E4;
	// b 0x8212e954
	return;
loc_8207D9E8:
	// li r3,166
	v_r3.s64 = 166;
	// bl 0x820826d8
	ctx.lr = 0x8207D9F0;
	ctx.r3 = v_r3;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f2,f31
	ctx.fpscr.disableFlushMode();
	v_f2.f64 = f31.f64;
	// addi r8,r11,18812
	v_r8.s64 = r11.s64 + 18812;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// mr r10,r31
	v_r10.u64 = r31.u64;
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// lfs f4,3352(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3352);
	v_f4.f64 = double(temp.f32);
	// fmr f3,f4
	v_f3.f64 = v_f4.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// bl 0x820764c0
	ctx.lr = 0x8207DA3C;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820764C0(ctx, base);
	// addi r1,r1,1984
	ctx.r1.s64 = ctx.r1.s64 + 1984;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e8bc
	ctx.lr = 0x8207DA48;
	// b 0x8212e954
	return;
loc_8207DA4C:
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11.s32, 0, xer);
	// bne cr6,0x8207db4c
	if (!cr6.eq) goto loc_8207DB4C;
	// lwz r11,44(r26)
	r11.u64 = REX_LOAD_U32(r26.u32 + 44);
	// rlwinm r11,r11,0,22,22
	r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 0) & 0x200;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f0,14980(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 14980);
	f0.f64 = double(temp.f32);
	// lfs f13,1740(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 1740);
	v_f13.f64 = double(temp.f32);
	// fcmpu cr6,f0,f13
	cr6.compare(f0.f64, v_f13.f64);
	// ble cr6,0x8207dd1c
	if (!cr6.gt) goto loc_8207DD1C;
	// fneg f13,f0
	v_f13.u64 = f0.u64 ^ 0x8000000000000000;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fsubs f12,f0,f24
	v_f12.f64 = double(float(f0.f64 - f24.f64));
	// lwz r31,14976(r31)
	r31.u64 = REX_LOAD_U32(r31.u32 + 14976);
	// cmpwi cr6,r31,0
	cr6.compare<int32_t>(r31.s32, 0, xer);
	// fsel f13,f13,f23,f0
	v_f13.f64 = v_f13.f64 >= 0.0 ? f23.f64 : f0.f64;
	// lfd f0,3192(r11)
	f0.u64 = REX_LOAD_U64(r11.u32 + 3192);
	// fsel f0,f12,f0,f13
	f0.f64 = v_f12.f64 >= 0.0 ? f0.f64 : v_f13.f64;
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// bge cr6,0x8207daa8
	if (!cr6.lt) goto loc_8207DAA8;
	// mr r31,r15
	r31.u64 = r15.u64;
	// b 0x8207dab4
	goto loc_8207DAB4;
loc_8207DAA8:
	// cmpwi cr6,r31,75
	cr6.compare<int32_t>(r31.s32, 75, xer);
	// ble cr6,0x8207dab4
	if (!cr6.gt) goto loc_8207DAB4;
	// li r31,75
	r31.s64 = 75;
loc_8207DAB4:
	// lfs f13,32(r30)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r30.u32 + 32);
	v_f13.f64 = double(temp.f32);
	// li r3,167
	v_r3.s64 = 167;
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,208(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 208, temp.u32);
	// lfs f13,36(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 36);
	v_f13.f64 = double(temp.f32);
	// fmuls f13,f13,f0
	v_f13.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f13,212(r1)
	temp.f32 = float(v_f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 212, temp.u32);
	// lfs f13,40(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 40);
	v_f13.f64 = double(temp.f32);
	// fmuls f0,f13,f0
	f0.f64 = double(float(v_f13.f64 * f0.f64));
	// stfs f0,216(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 216, temp.u32);
	// lfs f0,44(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 44);
	f0.f64 = double(temp.f32);
	// stfs f0,220(r1)
	temp.f32 = float(f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 220, temp.u32);
	// bl 0x820826d8
	ctx.lr = 0x8207DAE8;
	ctx.f12 = v_f12; ctx.f13 = v_f13; ctx.r3 = v_r3;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f2,f31
	ctx.fpscr.disableFlushMode();
	v_f2.f64 = f31.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// addi r7,r1,208
	v_r7.s64 = ctx.r1.s64 + 208;
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// mr r10,r31
	v_r10.u64 = r31.u64;
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// lfs f4,3352(r11)
	temp.u32 = REX_LOAD_U32(r11.u32 + 3352);
	v_f4.f64 = double(temp.f32);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// fmr f3,f4
	v_f3.f64 = v_f4.f64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// addi r8,r11,18820
	v_r8.s64 = r11.s64 + 18820;
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// li r11,75
	r11.s64 = 75;
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// stw r11,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, r11.u32);
	// bl 0x820764c0
	ctx.lr = 0x8207DB3C;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820764C0(ctx, base);
	// addi r1,r1,1984
	ctx.r1.s64 = ctx.r1.s64 + 1984;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e8bc
	ctx.lr = 0x8207DB48;
	// b 0x8212e954
	return;
loc_8207DB4C:
	// cmpwi cr6,r11,4
	cr6.compare<int32_t>(r11.s32, 4, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// lwz r11,44(r26)
	r11.u64 = REX_LOAD_U32(r26.u32 + 44);
	// lis r29,-32230
	r29.s64 = -2112225280;
	// lis r10,-32256
	v_r10.s64 = -2113929216;
	// rlwinm r11,r11,0,21,21
	r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 0) & 0x400;
	// mr r9,r15
	v_r9.u64 = r15.u64;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	// lis r11,-32256
	r11.s64 = -2113929216;
	// lfs f29,3352(r10)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(v_r10.u32 + 3352);
	f29.f64 = double(temp.f32);
	// lis r28,-32232
	r28.s64 = -2112356352;
	// addi r27,r11,18800
	r27.s64 = r11.s64 + 18800;
	// lwz r11,-2952(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + -2952);
	// bne cr6,0x8207dc48
	if (!cr6.eq) goto loc_8207DC48;
	// cmpwi cr6,r11,25
	cr6.compare<int32_t>(r11.s32, 25, xer);
	// blt cr6,0x8207dc48
	if (cr6.lt) goto loc_8207DC48;
	// extsw r10,r11
	v_r10.s64 = r11.s32;
	// lwz r11,7360(r28)
	r11.u64 = REX_LOAD_U32(r28.u32 + 7360);
	// addi r9,r1,96
	v_r9.s64 = ctx.r1.s64 + 96;
	// extsw r11,r11
	r11.s64 = r11.s32;
	// std r10,96(r1)
	REX_STORE_U64(ctx.r1.u32 + 96, v_r10.u64);
	// lfd f0,96(r1)
	f0.u64 = REX_LOAD_U64(ctx.r1.u32 + 96);
	// fcfid f0,f0
	f0.f64 = double(f0.s64);
	// std r11,128(r1)
	REX_STORE_U64(ctx.r1.u32 + 128, r11.u64);
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// lfd f13,128(r1)
	v_f13.u64 = REX_LOAD_U64(ctx.r1.u32 + 128);
	// fcfid f13,f13
	v_f13.f64 = double(v_f13.s64);
	// frsp f13,f13
	v_f13.f64 = double(float(v_f13.f64));
	// fdivs f0,f0,f13
	f0.f64 = double(float(f0.f64 / v_f13.f64));
	// fmuls f0,f0,f22
	f0.f64 = double(float(f0.f64 * f22.f64));
	// fctiwz f0,f0
	f0.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// stfiwx f0,0,r9
	REX_STORE_U32(v_r9.u32, f0.u32);
	// lwz r31,96(r1)
	r31.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// cmpwi cr6,r31,0
	cr6.compare<int32_t>(r31.s32, 0, xer);
	// bge cr6,0x8207dbe0
	if (!cr6.lt) goto loc_8207DBE0;
	// mr r31,r15
	r31.u64 = r15.u64;
	// b 0x8207dbec
	goto loc_8207DBEC;
loc_8207DBE0:
	// cmpwi cr6,r31,100
	cr6.compare<int32_t>(r31.s32, 100, xer);
	// ble cr6,0x8207dbec
	if (!cr6.gt) goto loc_8207DBEC;
	// li r31,100
	r31.s64 = 100;
loc_8207DBEC:
	// li r3,168
	v_r3.s64 = 168;
	// bl 0x820826d8
	ctx.lr = 0x8207DBF4;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// fmr f4,f29
	ctx.fpscr.disableFlushMode();
	v_f4.f64 = f29.f64;
	// fmr f3,f29
	v_f3.f64 = f29.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// mr r8,r27
	v_r8.u64 = r27.u64;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// mr r10,r31
	v_r10.u64 = r31.u64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820764c0
	ctx.lr = 0x8207DC38;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820764C0(ctx, base);
	v_f13 = ctx.f13; v_r7 = ctx.r7; v_r8 = ctx.r8;
	// lis r11,-32256
	r11.s64 = -2113929216;
	// li r9,1
	v_r9.s64 = 1;
	// lfs f31,18796(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11.u32 + 18796);
	f31.f64 = double(temp.f32);
	// lwz r11,-2952(r29)
	r11.u64 = REX_LOAD_U32(r29.u32 + -2952);
loc_8207DC48:
	// lwz r10,44(r26)
	v_r10.u64 = REX_LOAD_U32(r26.u32 + 44);
	// rlwinm r10,r10,0,20,20
	v_r10.u64 = __builtin_rotateleft64(v_r10.u32 | (v_r10.u64 << 32), 0) & 0x800;
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(v_r10.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// clrlwi r10,r9,24
	v_r10.u64 = v_r9.u32 & 0xFF;
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(v_r10.u32, 0, xer);
	// bne cr6,0x8207dd1c
	if (!cr6.eq) goto loc_8207DD1C;
	// cmpwi cr6,r11,25
	cr6.compare<int32_t>(r11.s32, 25, xer);
	// blt cr6,0x8207dd1c
	if (cr6.lt) goto loc_8207DD1C;
	// extsw r11,r11
	r11.s64 = r11.s32;
	// lwz r10,7360(r28)
	v_r10.u64 = REX_LOAD_U32(r28.u32 + 7360);
	// addi r9,r1,96
	v_r9.s64 = ctx.r1.s64 + 96;
	// rlwinm r10,r10,1,0,30
	v_r10.u64 = __builtin_rotateleft64(v_r10.u32 | (v_r10.u64 << 32), 1) & 0xFFFFFFFE;
	// extsw r10,r10
	v_r10.s64 = v_r10.s32;
	// std r11,128(r1)
	REX_STORE_U64(ctx.r1.u32 + 128, r11.u64);
	// std r10,96(r1)
	REX_STORE_U64(ctx.r1.u32 + 96, v_r10.u64);
	// lfd f13,96(r1)
	ctx.fpscr.disableFlushMode();
	v_f13.u64 = REX_LOAD_U64(ctx.r1.u32 + 96);
	// fcfid f13,f13
	v_f13.f64 = double(v_f13.s64);
	// frsp f13,f13
	v_f13.f64 = double(float(v_f13.f64));
	// lfd f0,128(r1)
	f0.u64 = REX_LOAD_U64(ctx.r1.u32 + 128);
	// fcfid f0,f0
	f0.f64 = double(f0.s64);
	// frsp f0,f0
	f0.f64 = double(float(f0.f64));
	// fdivs f0,f0,f13
	f0.f64 = double(float(f0.f64 / v_f13.f64));
	// fmuls f0,f0,f22
	f0.f64 = double(float(f0.f64 * f22.f64));
	// fctiwz f0,f0
	f0.s64 = __builtin_isnan(f0.f64) ? int64_t(0x80000000U) : (f0.f64 >= double(INT_MAX)) ? INT_MAX : gw_trunc_i32(f0.f64);
	// stfiwx f0,0,r9
	REX_STORE_U32(v_r9.u32, f0.u32);
	// lwz r31,96(r1)
	r31.u64 = REX_LOAD_U32(ctx.r1.u32 + 96);
	// cmpwi cr6,r31,0
	cr6.compare<int32_t>(r31.s32, 0, xer);
	// bge cr6,0x8207dcc4
	if (!cr6.lt) goto loc_8207DCC4;
	// mr r31,r15
	r31.u64 = r15.u64;
	// b 0x8207dcd0
	goto loc_8207DCD0;
loc_8207DCC4:
	// cmpwi cr6,r31,100
	cr6.compare<int32_t>(r31.s32, 100, xer);
	// ble cr6,0x8207dcd0
	if (!cr6.gt) goto loc_8207DCD0;
	// li r31,100
	r31.s64 = 100;
loc_8207DCD0:
	// li r3,169
	v_r3.s64 = 169;
	// bl 0x820826d8
	ctx.lr = 0x8207DCD8;
	ctx.f13 = v_f13; ctx.r3 = v_r3; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820826D8(ctx, base);
	v_r3 = ctx.r3;
	// fmr f4,f29
	ctx.fpscr.disableFlushMode();
	v_f4.f64 = f29.f64;
	// fmr f3,f29
	v_f3.f64 = f29.f64;
	// mr r9,r3
	v_r9.u64 = v_r3.u64;
	// fmr f2,f31
	v_f2.f64 = f31.f64;
	// mr r8,r27
	v_r8.u64 = r27.u64;
	// fmr f1,f30
	v_f1.f64 = f30.f64;
	// addi r7,r30,32
	v_r7.s64 = r30.s64 + 32;
	// mr r10,r31
	v_r10.u64 = r31.u64;
	// stfs f4,44(r1)
	temp.f32 = float(v_f4.f64);
	REX_STORE_U32(ctx.r1.u32 + 44, temp.u32);
	// stfs f3,36(r1)
	temp.f32 = float(v_f3.f64);
	REX_STORE_U32(ctx.r1.u32 + 36, temp.u32);
	// ld r6,44(r1)
	v_r6.u64 = REX_LOAD_U64(ctx.r1.u32 + 44);
	// stfs f2,28(r1)
	temp.f32 = float(v_f2.f64);
	REX_STORE_U32(ctx.r1.u32 + 28, temp.u32);
	// ld r5,36(r1)
	v_r5.u64 = REX_LOAD_U64(ctx.r1.u32 + 36);
	// stfs f1,20(r1)
	temp.f32 = float(v_f1.f64);
	REX_STORE_U32(ctx.r1.u32 + 20, temp.u32);
	// ld r4,28(r1)
	v_r4.u64 = REX_LOAD_U64(ctx.r1.u32 + 28);
	// ld r3,20(r1)
	v_r3.u64 = REX_LOAD_U64(ctx.r1.u32 + 20);
	// bl 0x820764c0
	ctx.lr = 0x8207DD1C;
	ctx.f1 = v_f1; ctx.f2 = v_f2; ctx.f3 = v_f3; ctx.f4 = v_f4; ctx.r3 = v_r3; ctx.r4 = v_r4; ctx.r5 = v_r5; ctx.r6 = v_r6; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	sub_820764C0(ctx, base);
	v_f13 = ctx.f13; v_r7 = ctx.r7; v_r8 = ctx.r8; v_r9 = ctx.r9; v_r10 = ctx.r10;
loc_8207DD1C:
	// addi r1,r1,1984
	ctx.r1.s64 = ctx.r1.s64 + 1984;
	// addi r12,r1,-144
	r12.s64 = ctx.r1.s64 + -144;
	// bl 0x8212e8bc
	ctx.lr = 0x8207DD28;
	// b 0x8212e954
	ctx.f13 = v_f13; ctx.r7 = v_r7; ctx.r8 = v_r8; ctx.r9 = v_r9; ctx.r10 = v_r10;
	return;

}
#endif  // GW_BIG_NATIVE
