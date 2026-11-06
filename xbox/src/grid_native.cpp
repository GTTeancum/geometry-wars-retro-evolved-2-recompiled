// Native replacement for the game's background grid update (sub_8205B880),
// which took about a third of the Xbox's CPU time as recompiled code.
//
// The grid is 133 points wide; the game's job threads each take one row at a
// time from a shared counter (88 rows). Per row the function:
//   1. moves the 131 inner points: pushes from a list of pushers in the job
//      argument, from the objects in the point's spatial-hash cell and from
//      two optional extra pushers; springs to its rest position and to its
//      four neighbours' displacements; damping; clamped to the play area.
//      Positions are double-buffered (read slot W, write slot idx).
//   2. writes the row's lines as thin quads (1052 vertices of 28 bytes:
//      x, y, z, u, v, colour, edge) into the line vertex buffer;
//   3. writes the row's displacements (133 vertices) into the buffer the
//      grid's distortion pass draws.
//
// Point layout (40 bytes, big-endian floats): +0 velocity x, y; +8 position
// slot 0 x, y; +16 slot 1 x, y; +24 rest x, y; +32 texture u, v.
//
// The maths follows the PowerPC code instruction by instruction, in single
// precision (the game rounds every result to single anyway), with SSE's
// reciprocal square root estimate standing in for frsqrte.
//
// GW_GRID_CHECK (make GRID_CHECK=y): once every 600 calls, run the original
// and the native code on the same input and log how far they differ.
#include "runtime.h"

#include <windows.h>
#include <xmmintrin.h>

extern "C" void __imp__sub_8205B880(PPCContext& ctx, uint8_t* base);

namespace {

constexpr u32 C = 0x82000000, D = 0x82180000, K = 0x821A0000;
constexpr u32 kCounter = K + 5892;
constexpr u32 kRows = 88, kCols = 133, kPoint = 40, kRow = kCols * kPoint;  // 5320
constexpr u32 kLineVertices = 4 + 131 * 8, kVertex = 28;

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

// A word copied as is (the colours are stored as float-typed words).
inline u32 lw(u32 a) { return gw_load32(a); }
inline u32 raw(u32 a) {
  u32 v;
  memcpy(&v, gw_host(a), 4);
  return v;
}
inline void sraw(u32 a, u32 v) { memcpy(gw_host(a), &v, 4); }

inline float ld(u32 a) {  // a double constant, as float
  u64 v = gw_load64(a);
  double d;
  memcpy(&d, &v, 8);
  return (float)d;
}

// frsqrte is an estimate on the 360 too; the recompiled code computes it
// exactly. tools/grid_check builds with GW_GRID_EXACT_RSQ to compare like
// with like.
#ifdef GW_GRID_EXACT_RSQ
inline float rsq(float x) { return (float)(1.0 / __builtin_sqrt((double)x)); }
#else
inline float rsq(float x) { return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x))); }
#endif
inline float sel(float a, float b, float c) { return a >= 0.0f ? b : c; }  // fsel

// Takes the next row from the shared counter (lwarx/stwcx. increment).
u32 next_row() {
  volatile u32* p = (volatile u32*)gw_host(kCounter);
  for (;;) {
    u32 old = *p;
    u32 value = __builtin_bswap32(old) + 1;
    if (__sync_bool_compare_and_swap(p, old, __builtin_bswap32(value))) return value;
  }
}

struct Constants {
  float push_b, tiny, damping, stiffness, neighbour, push_c, step, object_keep, radius_k, initial;
  float edge_a, edge_b, thickness_bias, reach, zero, bias, falloff;
  u32 colours, z_src, width_src;
};

Constants load_constants() {
  Constants k;
  k.push_b = ld(C + 10144);
  k.tiny = ld(C + 10128);
  k.damping = lf(C + 4468);
  k.stiffness = lf(C + 1824);
  k.neighbour = lf(C + 10120);
  k.push_c = ld(C + 4624);
  k.step = ld(C + 10136);
  k.object_keep = ld(C + 10112);
  k.radius_k = ld(C + 10104);  // f3
  k.initial = lf(C + 1740);
  k.edge_a = lf(C + 10092);  // f21
  k.edge_b = lf(C + 10088);  // f22
  k.thickness_bias = ld(C + 10080);  // f18
  k.reach = ld(C + 3192);  // f23
  k.zero = ld(C + 3376);  // f20
  k.bias = lf(C + 9080);  // f19
  k.falloff = ld(C + 10096);  // f4
  return k;
}

inline void vertex(u32 out, float x, float y, u32 z, u32 u, u32 v, u32 colour, u32 edge) {
  sf(out + 0, x);
  sf(out + 4, y);
  sraw(out + 8, z);
  sraw(out + 12, u);
  sraw(out + 16, v);
  sraw(out + 20, colour);
  sraw(out + 24, edge);
}

// One grid row in host floats.
struct RowCache {
  float x[kCols], y[kCols];            // position (read slot)
  float rest_x[kCols], rest_y[kCols];  // rest position
  float dx[kCols], dy[kCols];          // position - rest
  void load(u32 row, u32 read) {
    for (u32 i = 0; i < kCols; ++i) {
      const u32 p = row + i * kPoint;
      x[i] = lf(p + 8 + read);
      y[i] = lf(p + 12 + read);
      rest_x[i] = lf(p + 24);
      rest_y[i] = lf(p + 28);
      dx[i] = x[i] - rest_x[i];
      dy[i] = y[i] - rest_y[i];
    }
  }
};

void update_row(u32 args, u32 j, const Constants& k) {
  const u32 grid = lw(K + 5896);
  const int pushers = (int)lw(args + 0);
  const bool push_b = gw_load8(args + 4) != 0, push_c = gw_load8(args + 5) != 0;
  // The spatial hash of the game's objects (K + 7472): +4 objects, +8 cell table.
  const u32 cells = lw(K + 7472 + 8), objects = lw(K + 7472 + 4);
  const float max_x = lf(D + 9652), max_y = lf(D + 9656);
  const u32 read = lw(D + 9660) * 8, write = lw(K + 5908) * 8;
  const u32 cells_w = lw(K + 1556), cells_h = lw(K + 1560);
  const float cell_ox = lf(K + 1540), cell_oy = lf(K + 1544), cell_sx = lf(D + 9640), cell_sy = lf(D + 9644);
  const u32 row = grid + j * kRow;

  // This row and its neighbours, unpacked once: positions (the read slot,
  // which no job writes this frame) and displacements from rest.
  RowCache above, here, below;
  above.load(row - kRow, read);
  here.load(row, read);
  below.load(row + kRow, read);

  // 1. Physics of the inner points.
  for (u32 i = 1; i <= 131; ++i) {
    const u32 p = row + i * kPoint;
    const float px = here.x[i], py = here.y[i];
    float fx = k.initial, fy = k.initial;
    const int cx = (int)((cell_ox + px) * cell_sx), cy = (int)((cell_oy + py) * cell_sy);

    u32 q = args + 32;
    for (int n = 0; n < pushers; ++n, q += 16) {
      const float dx = lf(q) - px, dy = lf(q + 4) - py;
      const float r = rsq(dx * dx + dy * dy + k.bias);
      const float a = r * k.falloff;
      float s = lf(q + 8);
      s = sel(s - a, a, s) * lf(q + 12) * r;
      s = sel(r - k.radius_k, s, k.zero);
      const float nx = dx * s + fx, ny = dy * s + fy;
      const float gate = r - k.reach;
      fx = sel(gate, fx, nx);
      fy = sel(gate, fy, ny);
    }

    if ((u32)cx < cells_w && (u32)cy < cells_h) {
      const u32 cell = cells + (cells_w * (u32)cy + (u32)cx) * 4;
      const int first = (int)lw(cell), last = (int)lw(cell + 4);
      for (int n = first; n < last; ++n) {
        const u32 o = objects + (u32)n * 20 + 4;
        const float dx = px - lf(o), dy = py - lf(o + 4);
        const float kx = fx * k.object_keep, ky = fy * k.object_keep;
        const float d2 = dx * dx + dy * dy;
        const float inside = d2 - lf(o + 8);
        const float s = rsq(sel(d2 - k.reach, d2, k.reach)) * lf(o + 12);
        const float nx = dx * s + kx, ny = dy * s + ky;
        fx = sel(inside, fx, nx);
        fy = sel(inside, fy, ny);
      }
    }

    if (push_b) {
      const float dx = lf(args + 16) - px, dy = lf(args + 20) - py;
      const float r = rsq(dx * dx + dy * dy + k.bias);
      const float s = r * k.push_b, gate = r - k.reach;
      fx = sel(gate, fx, dx * s + fx);
      fy = sel(gate, fy, dy * s + fy);
    }
    if (push_c) {
      const float dx = lf(args + 24) - px, dy = lf(args + 28) - py;
      const float r = rsq(dx * dx + dy * dy + k.bias);
      const float s = r * k.push_c, gate = r - k.reach;
      fx = sel(gate, fx, fx - dx * s);
      fy = sel(gate, fy, fy - dy * s);
    }

    // Springs: the neighbours' displacements from rest (above + below, then
    // left, then right, the PowerPC order).
    float sx = (above.dx[i] + below.dx[i]) + here.dx[i - 1] + here.dx[i + 1];
    float sy = (above.dy[i] + below.dy[i]) + here.dy[i - 1] + here.dy[i + 1];
    sx *= k.neighbour;
    sy *= k.neighbour;
    float vx = (here.rest_x[i] + sx - px) * k.stiffness + lf(p + 0);
    float vy = (here.rest_y[i] + sy - py) * k.stiffness + lf(p + 4);
    vx = (vx + fx) * k.damping;
    vy = (vy + fy) * k.damping;
    vx = sel(__builtin_fabsf(vx) - k.tiny, vx, k.zero);
    vy = sel(__builtin_fabsf(vy) - k.tiny, vy, k.zero);
    sf(p + 0, vx);
    sf(p + 4, vy);
    float x = vx * k.step + px, y = vy * k.step + py;
    x = sel(-max_x - x, -max_x, x);
    x = sel(x - max_x, max_x, x);
    y = sel(-max_y - y, -max_y, y);
    y = sel(y - max_y, max_y, y);
    sf(p + 8 + write, x);
    sf(p + 12 + write, y);
  }

  // 2. The row's lines: thin quads along each segment.
  const float thickness = lf(K + 6964) * lf(K + 5900);
  const u32 z = raw(K + 7016);
  const u32 edge_a = raw(C + 10092), edge_b = raw(C + 10088);
  const u32 row_colour = raw(K + 5852 + (j & 7) * 4);
  auto segment = [&](u32 out, float x0, float y0, u32 a, float x1, float y1, u32 b, u32 colour) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float s = rsq(dx * dx + dy * dy + k.thickness_bias) * thickness;
    const float nx = s * dy, ny = s * dx;
    vertex(out + 0 * kVertex, x0 - nx, y0 + ny, z, raw(a + 32), raw(a + 36), colour, edge_b);
    vertex(out + 1 * kVertex, x0 + nx, y0 - ny, z, raw(a + 32), raw(a + 36), colour, edge_a);
    vertex(out + 2 * kVertex, x1 + nx, y1 - ny, z, raw(b + 32), raw(b + 36), colour, edge_a);
    vertex(out + 3 * kVertex, x1 - nx, y1 + ny, z, raw(b + 32), raw(b + 36), colour, edge_b);
  };
  auto pos_x = [&](u32 p) { return lf(p + 8 + read); };
  auto pos_y = [&](u32 p) { return lf(p + 12 + read); };
  const u32 lines = lw(K + 5888);
  u32 out = lines + j * kLineVertices * kVertex - 14784;
  segment(out, here.x[0], here.y[0], row, here.x[1], here.y[1], row + kPoint, row_colour);
  out += 4 * kVertex;
  for (u32 i = 1; i <= 131; ++i) {
    const u32 p = row + i * kPoint;
    const u32 column_colour = raw(K + 5852 + (i & 7) * 4);
    segment(out, here.x[i], here.y[i], p, here.x[i + 1], here.y[i + 1], p + kPoint, row_colour);
    segment(out + 4 * kVertex, here.x[i], here.y[i], p, below.x[i], below.y[i], p + kRow, column_colour);
    out += 8 * kVertex;
  }
  // Row 1's job also draws the uprights between rows 0 and 1, at the start
  // of the buffer.
  if (j == 1) {
    u32 o = lines;
    for (u32 i = 1; i <= 131; ++i) {
      const u32 p = grid + i * kPoint, below = p + kRow;
      segment(o, pos_x(p), pos_y(p), p, pos_x(below), pos_y(below), below, raw(K + 5852 + (i & 7) * 4));
      o += 4 * kVertex;
    }
  }

  // 3. Displacements for the distortion pass.
  const u32 displacement = lw(K + 5884);
  const u32 zero_word = raw(C + 1740);
  auto displacements = [&](u32 out, u32 from) {
    for (u32 i = 0; i < kCols; ++i, from += kPoint, out += kVertex) {
      sf(out + 0, pos_x(from) - lf(from + 24));
      sf(out + 4, pos_y(from) - lf(from + 28));
      sraw(out + 8, z);
      sraw(out + 12, raw(from + 32));
      sraw(out + 16, raw(from + 36));
      sraw(out + 20, zero_word);
      sraw(out + 24, zero_word);
    }
  };
  if (j == 1) displacements(displacement, grid);
  out = displacement + j * kCols * kVertex;
  for (u32 i = 0; i < kCols; ++i, out += kVertex) {
    const u32 p = row + i * kPoint;
    sf(out + 0, here.dx[i]);
    sf(out + 4, here.dy[i]);
    sraw(out + 8, z);
    sraw(out + 12, raw(p + 32));
    sraw(out + 16, raw(p + 36));
    sraw(out + 20, zero_word);
    sraw(out + 24, zero_word);
  }
  if (j == 87) displacements(displacement + 88 * kCols * kVertex, row + kRow);
}

void update_grid(u32 args) {
  const Constants k = load_constants();
  for (u32 j; (j = next_row()) < kRows;) update_row(args, j, k);
}

#ifdef GW_GRID_CHECK
CRITICAL_SECTION g_check_lock;
LONG g_check_ready;
u32 g_calls;

struct Region {
  u32 guest, size;
};

void compare(const char* what, const u8* want, const u8* got, u32 size) {
  float worst = 0;
  u32 worst_at = 0, bad = 0;
  for (u32 o = 0; o + 4 <= size; o += 4) {
    u32 a, b;
    memcpy(&a, want + o, 4);
    memcpy(&b, got + o, 4);
    if (a == b) continue;
    a = __builtin_bswap32(a);
    b = __builtin_bswap32(b);
    float fa, fb;
    memcpy(&fa, &a, 4);
    memcpy(&fb, &b, 4);
    float err = __builtin_fabsf(fa - fb) / (1.0f + __builtin_fabsf(fa));
    if (!(err <= 1e-3f)) ++bad;
    if (!(err <= worst)) {
      worst = err;
      worst_at = o;
    }
  }
  gw_log("grid check %s: %u of %u words off by more than 0.1%%, worst %g at +%X\n", what, bad, size / 4, worst,
         worst_at);
}

// Runs the original on the current input, then the native code on the same
// input, and compares everything both wrote. Other job threads wait.
bool check(PPCContext& ctx, uint8_t* base) {
  if (InterlockedCompareExchange(&g_check_ready, 1, 0) == 0) InitializeCriticalSection(&g_check_lock);
  EnterCriticalSection(&g_check_lock);
  bool ran = false;
  if (++g_calls % 600 == 1) {
    const u32 grid = lw(K + 5896);
    const Region regions[3] = {
        {grid - kRow, (kRows + 3) * kRow},
        {lw(K + 5888) - 14784, kRows * kLineVertices * kVertex},
        {lw(K + 5884), (kRows + 1) * kCols * kVertex},
    };
    u8* before[3];
    u8* want[3];
    for (int r = 0; r < 3; ++r) {
      before[r] = (u8*)malloc(regions[r].size);
      want[r] = (u8*)malloc(regions[r].size);
      memcpy(before[r], gw_host(regions[r].guest), regions[r].size);
    }
    const u32 counter = raw(kCounter), args = ctx.r3.u32;  // the original changes r3
    __imp__sub_8205B880(ctx, base);
    for (int r = 0; r < 3; ++r) {
      memcpy(want[r], gw_host(regions[r].guest), regions[r].size);
      memcpy(gw_host(regions[r].guest), before[r], regions[r].size);
    }
    sraw(kCounter, counter);
    update_grid(args);
    static const char* kNames[3] = {"points", "lines", "displacements"};
    for (int r = 0; r < 3; ++r) {
      compare(kNames[r], want[r], gw_host(regions[r].guest), regions[r].size);
      free(before[r]);
      free(want[r]);
    }
    ran = true;
  }
  LeaveCriticalSection(&g_check_lock);
  return ran;
}
#endif

}  // namespace

// The game's grid job (guest 0x8205B880, run by each job thread): r3 is the
// job argument (pushers, extra pushers). The name is what the recompiled
// callers link against.
extern "C" void sub_8205B880(PPCContext& ctx, uint8_t* base) {
#ifdef GW_GRID_CHECK
  if (check(ctx, base)) return;
#endif
  (void)base;
  update_grid(ctx.r3.u32);
}
