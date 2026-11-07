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
// The points live in host floats (HostGrid below), loaded from the guest
// grid when the game creates or resets it: nothing else reads or writes them
// (the creator sub_8205D118, the reset sub_8205B000 and this job are the only
// code that touches the points), and converting every value from and to
// big-endian on the Xbox's Pentium III cost more than the physics (an
// integer byte swap feeding an SSE register goes through memory). The guest
// copy keeps the positions of the last load. GW_GRID_GUEST_SYNC (the PC check,
// GRID_CHECK) loads from and stores back to the guest on every update.
//
// GW_GRID_CHECK (make GRID_CHECK=y): once every 600 calls, run the original
// and the native code on the same input and log how far they differ.
#include "runtime.h"

#include <windows.h>
#include <xmmintrin.h>

extern "C" void __imp__sub_8205B880(PPCContext& ctx, uint8_t* base);
#ifndef GW_GRID_GUEST_SYNC
extern "C" void __imp__sub_8205D118(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8205B000(PPCContext& ctx, uint8_t* base);
#endif

namespace {

constexpr u32 C = 0x82000000, D = 0x82180000, K = 0x821A0000;
constexpr u32 kCounter = K + 5892;
constexpr u32 kRows = 88, kCols = 133, kPoint = 40, kRow = kCols * kPoint;  // 5320
constexpr u32 kLineVertices = 4 + 131 * 8, kVertex = 28;
// The displacements feed only the grid's distortion pass, which src/nv2a.cpp
// does not draw yet: until it does they are not written.
#ifdef GW_GRID_VERTICES
constexpr bool kWriteDisplacements = true;
#else
constexpr bool kWriteDisplacements = false;
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

// A line vertex in the guest buffer: x, y, z, u, v, colour, edge (z on are
// guest words). Only without the host mirror (update_row's segment).
inline void vertex(u32 out, float x, float y, u32 z, u32 u, u32 v, u32 colour, u32 edge) {
  sf(out + 0, x);
  sf(out + 4, y);
  sraw(out + 8, z);
  sraw(out + 12, u);
  sraw(out + 16, v);
  sraw(out + 20, colour);
  sraw(out + 24, edge);
}

// The grid points in host floats, both position slots. The game updates
// rows 1 to 87 (rows 0 and 88, and the first and last column, are the fixed
// border); the PC check also runs row 0, which reads row -1 (before the
// grid). Array row r holds grid row r - 1: rows -1 to 89, the outer two
// loaded only with GW_GRID_GUEST_SYNC.
constexpr u32 kGridRows = kRows + 3;
#ifdef GW_GRID_GUEST_SYNC
constexpr int kFirstRow = -1, kLastRow = kRows + 1;
#else
constexpr int kFirstRow = 0, kLastRow = kRows;
#endif
struct HostGrid {
  u32 guest;  // the guest grid loaded, 0 = load before the next update
  float pos_x[2][kGridRows][kCols], pos_y[2][kGridRows][kCols];
  float vel_x[kGridRows][kCols], vel_y[kGridRows][kCols];
  float rest_x[kGridRows][kCols], rest_y[kGridRows][kCols];

  void load(u32 grid) {
    for (int r = kFirstRow; r <= kLastRow; ++r) {
      const u32 j = (u32)(r + 1);
      for (u32 i = 0; i < kCols; ++i) {
        const u32 p = grid + (u32)r * kRow + i * kPoint;
        vel_x[j][i] = lf(p + 0);
        vel_y[j][i] = lf(p + 4);
        pos_x[0][j][i] = lf(p + 8);
        pos_y[0][j][i] = lf(p + 12);
        pos_x[1][j][i] = lf(p + 16);
        pos_y[1][j][i] = lf(p + 20);
        rest_x[j][i] = lf(p + 24);
        rest_y[j][i] = lf(p + 28);
      }
    }
    guest = grid;
  }
  void store(u32 grid) const {
    for (int r = kFirstRow; r <= kLastRow; ++r) {
      const u32 j = (u32)(r + 1);
      for (u32 i = 0; i < kCols; ++i) {
        const u32 p = grid + (u32)r * kRow + i * kPoint;
        sf(p + 0, vel_x[j][i]);
        sf(p + 4, vel_y[j][i]);
        sf(p + 8, pos_x[0][j][i]);
        sf(p + 12, pos_y[0][j][i]);
        sf(p + 16, pos_x[1][j][i]);
        sf(p + 20, pos_y[1][j][i]);
      }
    }
  }
};
HostGrid g_host;  // 390 KB

// The job's pushers (16 bytes each at args + 32: x, y, strength, scale),
// loaded once per job in host floats. A pusher moves a point only where
// its s is not zero: r >= radius_k with r = 1 / sqrt(d^2 + bias), so
// d^2 + bias <= 1 / radius_k^2. update_row skips, per row, the pushers too
// far above or below the whole row for that (2% margin for the estimated
// square root): they would add exactly zero. On the Xbox the pusher loop
// was most of the grid's time (bullets and explosions are pushers).
constexpr int kMaxPushers = 256;

// Logged every 600 grid updates: the pushers' and objects' work, and the
// time of the physics and of the lines per frame.
struct GridStats {
  u32 updates, pushers, active, pushes, objects, rows;
  u64 physics_cycles, line_cycles;
} g_stats;
struct Pushers {
  int count;
  float x[kMaxPushers], y[kMaxPushers], strength[kMaxPushers], scale[kMaxPushers];
  void load(u32 args) {
    count = (int)lw(args + 0);
    if (count > kMaxPushers) count = -1;  // too many: the rows read them in place
    for (int n = 0; n < count; ++n) {
      const u32 q = args + 32 + (u32)n * 16;
      x[n] = lf(q);
      y[n] = lf(q + 4);
      strength[n] = lf(q + 8);
      scale[n] = lf(q + 12);
    }
  }
};

void update_row(u32 args, u32 j, const Constants& k, const Pushers& all) {
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
  const u32 rs = read / 8, ws = write / 8;  // position slots
  HostGrid& g = g_host;
  const u32 h = j + 1;  // this row in the host arrays
  // This row and its neighbours: positions in the read slot (which no row
  // writes this frame), rest positions.
  const float* hx = g.pos_x[rs][h];
  const float* hy = g.pos_y[rs][h];
  // When the game reads and writes the same slot, each point still sees its
  // left neighbour's position from before this row (the original read the
  // row before writing any of it).
  float here_x[kCols], here_y[kCols];
  if (rs == ws) {
    memcpy(here_x, hx, sizeof(here_x));
    memcpy(here_y, hy, sizeof(here_y));
    hx = here_x;
    hy = here_y;
  }
  const float* ax = g.pos_x[rs][h - 1];
  const float* ay = g.pos_y[rs][h - 1];
  const float* bx = g.pos_x[rs][h + 1];
  const float* by = g.pos_y[rs][h + 1];
  const float* hrx = g.rest_x[h];
  const float* hry = g.rest_y[h];
  const float* arx = g.rest_x[h - 1];
  const float* ary = g.rest_y[h - 1];
  const float* brx = g.rest_x[h + 1];
  const float* bry = g.rest_y[h + 1];

  // The pushers that can reach this row, in their original order.
  float ymin = hy[1], ymax = hy[1];
  for (u32 i = 2; i <= 131; ++i) {
    ymin = hy[i] < ymin ? hy[i] : ymin;
    ymax = hy[i] > ymax ? hy[i] : ymax;
  }
  const float reach2 = k.radius_k > 0 ? 1.02f / (k.radius_k * k.radius_k) : 3.0e38f;
  int active[kMaxPushers];
  int active_count = 0;
  for (int n = 0; n < all.count; ++n) {
    const float py = all.y[n];
    const float dy = py < ymin ? ymin - py : py > ymax ? py - ymax : 0.0f;
    if (dy * dy + k.bias <= reach2) active[active_count++] = n;
  }
  g_stats.active += (u32)active_count;

  const u64 t1 = __builtin_ia32_rdtsc();
  // 1. Physics of the inner points. point(i) is one point, everything the
  // original does; springs4(i) is points i to i + 3 at once with SSE, for
  // when no pusher, extra pusher or object reaches them (the usual case:
  // only springs, damping and the clamp), in the same single-precision
  // operations in the same order, so the results are the same.
  auto point = [&](u32 i) {
    const float px = hx[i], py = hy[i];
    float fx = k.initial, fy = k.initial;
    const int cx = (int)((cell_ox + px) * cell_sx), cy = (int)((cell_oy + py) * cell_sy);

    auto push = [&](float qx, float qy, float strength, float scale) {
      const float dx = qx - px, dy = qy - py;
      const float d2 = dx * dx + dy * dy + k.bias;
      if (d2 > reach2) return;  // s would be 0 (see Pushers)
      ++g_stats.pushes;
      const float r = rsq(d2);
      const float a = r * k.falloff;
      float s = sel(strength - a, a, strength) * scale * r;
      s = sel(r - k.radius_k, s, k.zero);
      const float nx = dx * s + fx, ny = dy * s + fy;
      const float gate = r - k.reach;
      fx = sel(gate, fx, nx);
      fy = sel(gate, fy, ny);
    };
    if (all.count >= 0) {
      for (int m = 0; m < active_count; ++m) {
        const int n = active[m];
        push(all.x[n], all.y[n], all.strength[n], all.scale[n]);
      }
    } else {
      for (int n = 0; n < pushers; ++n) {
        const u32 q = args + 32 + (u32)n * 16;
        push(lf(q), lf(q + 4), lf(q + 8), lf(q + 12));
      }
    }

    if ((u32)cx < cells_w && (u32)cy < cells_h) {
      const u32 cell = cells + (cells_w * (u32)cy + (u32)cx) * 4;
      const int first = (int)lw(cell), last = (int)lw(cell + 4);
      for (int n = first; n < last; ++n) {
        const u32 o = objects + (u32)n * 20 + 4;
        const float dx = px - lf(o), dy = py - lf(o + 4);
        const float d2 = dx * dx + dy * dy;
        const float inside = d2 - lf(o + 8);
        ++g_stats.objects;
        if (inside >= 0) continue;  // outside the object's radius: no change
        const float kx = fx * k.object_keep, ky = fy * k.object_keep;
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
    float sx = ((ax[i] - arx[i]) + (bx[i] - brx[i])) + (hx[i - 1] - hrx[i - 1]) + (hx[i + 1] - hrx[i + 1]);
    float sy = ((ay[i] - ary[i]) + (by[i] - bry[i])) + (hy[i - 1] - hry[i - 1]) + (hy[i + 1] - hry[i + 1]);
    sx *= k.neighbour;
    sy *= k.neighbour;
    float vx = (hrx[i] + sx - px) * k.stiffness + g.vel_x[h][i];
    float vy = (hry[i] + sy - py) * k.stiffness + g.vel_y[h][i];
    vx = (vx + fx) * k.damping;
    vy = (vy + fy) * k.damping;
    vx = sel(__builtin_fabsf(vx) - k.tiny, vx, k.zero);
    vy = sel(__builtin_fabsf(vy) - k.tiny, vy, k.zero);
    g.vel_x[h][i] = vx;
    g.vel_y[h][i] = vy;
    float x = vx * k.step + px, y = vy * k.step + py;
    x = sel(-max_x - x, -max_x, x);
    x = sel(x - max_x, max_x, x);
    y = sel(-max_y - y, -max_y, y);
    y = sel(y - max_y, max_y, y);
    g.pos_x[ws][h][i] = x;
    g.pos_y[ws][h][i] = y;
  };

  // sel(a, b, c) = a >= 0 ? b : c, four at a time (NaN picks c, as fsel).
  auto sel4 = [](__m128 a, __m128 b, __m128 c) {
    const __m128 m = _mm_cmpge_ps(a, _mm_setzero_ps());
    return _mm_or_ps(_mm_and_ps(m, b), _mm_andnot_ps(m, c));
  };
  const __m128 initial4 = _mm_set1_ps(k.initial), neighbour4 = _mm_set1_ps(k.neighbour);
  const __m128 stiffness4 = _mm_set1_ps(k.stiffness), damping4 = _mm_set1_ps(k.damping);
  const __m128 tiny4 = _mm_set1_ps(k.tiny), zero4 = _mm_set1_ps(k.zero), step4 = _mm_set1_ps(k.step);
  const u32 abs_bits = 0x7FFFFFFF;  // SSE1 only (the Pentium III): the mask through a float
  float abs_float;
  memcpy(&abs_float, &abs_bits, 4);
  const __m128 abs_mask = _mm_set1_ps(abs_float);
  const __m128 max_x4 = _mm_set1_ps(max_x), max_y4 = _mm_set1_ps(max_y);
  const __m128 neg_max_x4 = _mm_set1_ps(-max_x), neg_max_y4 = _mm_set1_ps(-max_y);
  float* const vel_x = g.vel_x[h];
  float* const vel_y = g.vel_y[h];
  float* const out_x = g.pos_x[ws][h];
  float* const out_y = g.pos_y[ws][h];
  auto springs4 = [&](u32 i) {
    auto axis = [&](const float* hp, const float* hr, const float* ap, const float* ar, const float* bp,
                    const float* br, float* vel, float* out, __m128 hi, __m128 lo) {
      const __m128 pos = _mm_loadu_ps(hp + i);
      __m128 sum = _mm_add_ps(_mm_sub_ps(_mm_loadu_ps(ap + i), _mm_loadu_ps(ar + i)),
                              _mm_sub_ps(_mm_loadu_ps(bp + i), _mm_loadu_ps(br + i)));
      sum = _mm_add_ps(sum, _mm_sub_ps(_mm_loadu_ps(hp + i - 1), _mm_loadu_ps(hr + i - 1)));
      sum = _mm_add_ps(sum, _mm_sub_ps(_mm_loadu_ps(hp + i + 1), _mm_loadu_ps(hr + i + 1)));
      sum = _mm_mul_ps(sum, neighbour4);
      __m128 v = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(_mm_add_ps(_mm_loadu_ps(hr + i), sum), pos), stiffness4),
                            _mm_loadu_ps(vel + i));
      v = _mm_mul_ps(_mm_add_ps(v, initial4), damping4);
      v = sel4(_mm_sub_ps(_mm_and_ps(v, abs_mask), tiny4), v, zero4);
      _mm_storeu_ps(vel + i, v);
      __m128 p = _mm_add_ps(_mm_mul_ps(v, step4), pos);
      p = sel4(_mm_sub_ps(lo, p), lo, p);
      p = sel4(_mm_sub_ps(p, hi), hi, p);
      _mm_storeu_ps(out + i, p);
    };
    axis(hx, hrx, ax, arx, bx, brx, vel_x, out_x, max_x4, neg_max_x4);
    axis(hy, hry, ay, ary, by, bry, vel_y, out_y, max_y4, neg_max_y4);
  };
  // Whether an object's spatial-hash cell holds anything at point i.
  auto objects_at = [&](u32 i) {
    const int cx = (int)((cell_ox + hx[i]) * cell_sx), cy = (int)((cell_oy + hy[i]) * cell_sy);
    if ((u32)cx >= cells_w || (u32)cy >= cells_h) return false;
    const u32 cell = cells + (cells_w * (u32)cy + (u32)cx) * 4;
    return (int)lw(cell) < (int)lw(cell + 4);
  };

  const bool springs_only = active_count == 0 && (all.count >= 0 || pushers == 0) && !push_b && !push_c;
  u32 i = 1;
  if (springs_only) {
    for (; i + 3 <= 131; i += 4) {
      if (objects_at(i) || objects_at(i + 1) || objects_at(i + 2) || objects_at(i + 3)) {
        for (u32 q = i; q < i + 4; ++q) point(q);
      } else {
        springs4(i);
      }
    }
  }
  for (; i <= 131; ++i) point(i);
  ++g_stats.rows;
  const u64 t2 = __builtin_ia32_rdtsc();
  g_stats.physics_cycles += t2 - t1;
  struct LineTimer {
    u64 start;
    ~LineTimer() { g_stats.line_cycles += __builtin_ia32_rdtsc() - start; }
  } line_timer{t2};

  // 2. The row's lines. With the line buffer's host mirror (see
  // gw_grid_line_host), each segment is a line of two vertices there
  // (x, y, z, brightness, edge 0), the NV2A draws it; else, as the game
  // does, a thin quad of four vertices in the guest buffer.
  const float thickness = lf(K + 6964) * lf(K + 5900);
  const u32 z = raw(K + 7016);
  const u32 edge_a = raw(C + 10092), edge_b = raw(C + 10088);
  const u32 row_colour = raw(K + 5852 + (j & 7) * 4);
  const u32 lines = lw(K + 5888);
  // The buffer starts at 'lines' (row 1's uprights); row j's segments at
  // lines + j * 29456 - 14784, rows numbered from 1.
  u8* const mirror = gw_grid_line_host(lines);
  const u32 mirror_base = lines;
  const u32 hz = __builtin_bswap32(z);
  // a, b: the points' guest addresses (their texture u, v at +32, +36).
  auto segment = [&](u32 out, float x0, float y0, u32 a, float x1, float y1, u32 b, u32 colour) {
    if (mirror) {
      u32 w[10];
      memcpy(&w[0], &x0, 4);
      memcpy(&w[1], &y0, 4);
      w[2] = hz;
      w[3] = __builtin_bswap32(colour);
      w[4] = 0;
      memcpy(&w[5], &x1, 4);
      memcpy(&w[6], &y1, 4);
      w[7] = hz;
      w[8] = w[3];
      w[9] = 0;
      memcpy(mirror + (out - mirror_base) / (4 * kVertex) * 40, w, sizeof(w));
      return;
    }
    const float dx = x1 - x0, dy = y1 - y0;
    const float s = rsq(dx * dx + dy * dy + k.thickness_bias) * thickness;
    const float nx = s * dy, ny = s * dx;
    vertex(out + 0 * kVertex, x0 - nx, y0 + ny, z, raw(a + 32), raw(a + 36), colour, edge_b);
    vertex(out + 1 * kVertex, x0 + nx, y0 - ny, z, raw(a + 32), raw(a + 36), colour, edge_a);
    vertex(out + 2 * kVertex, x1 + nx, y1 - ny, z, raw(b + 32), raw(b + 36), colour, edge_a);
    vertex(out + 3 * kVertex, x1 - nx, y1 + ny, z, raw(b + 32), raw(b + 36), colour, edge_b);
  };
  u32 out = lines + j * kLineVertices * kVertex - 14784;
  segment(out, hx[0], hy[0], row, hx[1], hy[1], row + kPoint, row_colour);
  out += 4 * kVertex;
  for (u32 i = 1; i <= 131; ++i) {
    const u32 p = row + i * kPoint;
    const u32 column_colour = raw(K + 5852 + (i & 7) * 4);
    segment(out, hx[i], hy[i], p, hx[i + 1], hy[i + 1], p + kPoint, row_colour);
    segment(out + 4 * kVertex, hx[i], hy[i], p, bx[i], by[i], p + kRow, column_colour);
    out += 8 * kVertex;
  }
  // Row 1's job also draws the uprights between rows 0 and 1, at the start
  // of the buffer.
  if (j == 1) {
    u32 o = lines;
    for (u32 i = 1; i <= 131; ++i) {
      const u32 p = grid + i * kPoint, below = p + kRow;
      segment(o, ax[i], ay[i], p, hx[i], hy[i], below, raw(K + 5852 + (i & 7) * 4));
      o += 4 * kVertex;
    }
  }

  // 3. Displacements for the distortion pass.
  if (!kWriteDisplacements) return;
  const u32 displacement = lw(K + 5884);
  const u32 zero_word = raw(C + 1740);
  // Row r's points (r = 0 or 88: the border rows).
  auto displacements = [&](u32 out, u32 r) {
    u32 from = grid + r * kRow;
    for (u32 i = 0; i < kCols; ++i, from += kPoint, out += kVertex) {
      sf(out + 0, g.pos_x[rs][r + 1][i] - g.rest_x[r + 1][i]);
      sf(out + 4, g.pos_y[rs][r + 1][i] - g.rest_y[r + 1][i]);
      sraw(out + 8, z);
      sraw(out + 12, raw(from + 32));
      sraw(out + 16, raw(from + 36));
      sraw(out + 20, zero_word);
      sraw(out + 24, zero_word);
    }
  };
  if (j == 1) displacements(displacement, 0);
  out = displacement + j * kCols * kVertex;
  for (u32 i = 0; i < kCols; ++i, out += kVertex) {
    const u32 p = row + i * kPoint;
    sf(out + 0, hx[i] - hrx[i]);
    sf(out + 4, hy[i] - hry[i]);
    sraw(out + 8, z);
    sraw(out + 12, raw(p + 32));
    sraw(out + 16, raw(p + 36));
    sraw(out + 20, zero_word);
    sraw(out + 24, zero_word);
  }
  if (j == 87) displacements(displacement + 88 * kCols * kVertex, 88);
}

// The game runs the grid job on all six of its job threads, which take rows
// from a shared counter: on the 360's six hardware threads, in parallel. The
// Xbox has one core, so the threads only take turns (each switch costing
// the cache). The first thread in does every row; the others find it busy
// and return at once (the job system waits for all of them, so the frame
// still waits for the grid). Each row is still done exactly once.
volatile long g_grid_busy = 0;

void update_grid(u32 args) {
  if (InterlockedCompareExchange(&g_grid_busy, 1, 0) != 0) return;
  GwPartTimer timer(kPartGrid);
  const Constants k = load_constants();
  Pushers pushers;  // 4 KB on the job thread's stack
  pushers.load(args);
  g_stats.pushers += (u32)(pushers.count > 0 ? pushers.count : 0);
  if (++g_stats.updates % 600 == 0) {
    // Times per frame (87 rows), in microseconds at 733 MHz.
    const u64 frames_x733 = (u64)(g_stats.rows + 1) * 733 / 87;
    gw_log("grid: per update %u pushers, %u pusher-rows, %u pushes in reach, %u object tests; per frame "
           "physics %u us, lines %u us\n",
           g_stats.pushers / 600, g_stats.active / 600, g_stats.pushes / 600, g_stats.objects / 600,
           (u32)(g_stats.physics_cycles / frames_x733), (u32)(g_stats.line_cycles / frames_x733));
    g_stats = {g_stats.updates};
  }
  const u32 grid = lw(K + 5896);
#ifdef GW_GRID_GUEST_SYNC
  g_host.load(grid);
#else
  if (g_host.guest != grid) g_host.load(grid);
#endif
  for (u32 j; (j = next_row()) < kRows;) update_row(args, j, k, pushers);
#ifdef GW_GRID_GUEST_SYNC
  g_host.store(grid);
#endif
  InterlockedExchange(&g_grid_busy, 0);
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

#ifndef GW_GRID_GUEST_SYNC
// The grid's creator and its reset at the start of a level write the points:
// the host copy is loaded again before the next update.
extern "C" void sub_8205D118(PPCContext& ctx, uint8_t* base) {
  __imp__sub_8205D118(ctx, base);
  g_host.guest = 0;
}
extern "C" void sub_8205B000(PPCContext& ctx, uint8_t* base) {
  __imp__sub_8205B000(ctx, base);
  g_host.guest = 0;
}
#endif

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
