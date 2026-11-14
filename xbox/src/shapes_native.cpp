// Native replacement for the game's glowing-line builder (guest 0x8205FBA8).
//
// Every line of every vector shape the game draws (the ship, the enemies,
// the HUD's shapes) goes through this function once a frame: it turns the
// segment (x0, y0)-(x1, y1) into
//   - a thin quad (4 vertices) in the main picture's line stream, and
//   - an eight-vertex outline (two end caps and the sides) in the glow
//     stream,
// 32-byte vertices (x, y, z, u, r, g, b, w), the colour scaled from the
// line's colour (r7: r, g, b, a). It was the busiest of the game's own
// drawing functions on the Xbox's main thread.
//
// The arithmetic follows the PowerPC code instruction by instruction, with
// the recompiled code's conventions so the results are the same: single
// operations (fadds, fmuls) are rounded to single, double ones (fadd, fmul)
// kept in double, fmadd as a multiply then an add (gw_fma in the generated
// code), frsqrte as an exact reciprocal square root rounded to single,
// frsp/stfs rounding to single.
#include "runtime.h"

#include <xmmintrin.h>
#ifdef NXDK
#include <xboxkrnl/xboxkrnl.h>
#endif

extern "C" void sub_82061750(PPCContext& ctx, uint8_t* base);  // vertices from a dynamic buffer

namespace {

constexpr u32 C = 0x82000000, D = 0x82180000, K = 0x821A0000;

inline float lfs(u32 a) {
  u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline double lfd(u32 a) {
  u64 v = gw_load64(a);
  double d;
  memcpy(&d, &v, 8);
  return d;
}
inline void stfs(u32 a, double d) {
  const float f = (float)d;
  u32 v;
  memcpy(&v, &f, 4);
  gw_store32(a, v);
}
inline double single(double d) { return (double)(float)d; }  // fadds, fmuls, frsp

}  // namespace

// f1, f2 the start, f3, f4 the end; r7 the colour (four floats).
extern "C" void sub_8205FBA8(PPCContext& ctx, uint8_t* base) {
  const double x0 = ctx.f1.f64, y0 = ctx.f2.f64, x1 = ctx.f3.f64, y1 = ctx.f4.f64;
  const u32 colour = ctx.r7.u32;

  // Room for the vertices: 4 in the main stream, 8 in the glow stream. A
  // stream (guest 0x82061750's) fills locked chunks of 4096 vertices: +2052
  // the chunk's address, +2056 the vertices used. The usual case is taken
  // here; a full chunk goes to the game's code, which locks the next one.
  auto allocate = [&](u32 stream, u32 count) -> u32 {
    const u32 used = gw_load32(stream + 2056);
    if ((int)(used + count) <= 4096) {
      gw_store32(stream + 2056, used + count);
      return gw_load32(stream + 2052) + (used << 5);
    }
    ctx.r1.u64 = ctx.r1.u32 - 176;  // the original's frame
    ctx.r3.u64 = stream;
    ctx.r4.u64 = count;
    sub_82061750(ctx, base);
    ctx.r1.u64 = ctx.r1.u32 + 176;
    return ctx.r3.u32;
  };
  const u32 quad = allocate(K + 16648, 4);
  const u32 outline = allocate(K + 18760, 8);

  // The segment in screen space (the view's offset added), the colour scaled.
  const double offset_x = lfs(K + 14484 + 32), offset_y = lfs(K + 16596 + 32);
  const double ax = single(offset_x + x0), bx = single(offset_x + x1);  // f9, f7
  const double ay = single(offset_y + y0), by = single(offset_y + y1);  // f8, f6
  const double scale = lfs(C + 3352);
  const double red = single(lfs(colour + 0) * scale), green = single(lfs(colour + 4) * scale);
  const double blue = single(lfs(colour + 8) * scale), alpha = lfs(colour + 12);
  const double z = lfs(K + 18708 + 32), u0 = lfs(C + 3208), one = lfs(C + 1732), zero = lfs(C + 1740);
  const double width = single(single((double)lfs(K + 12372 + 32) * lfs(D + 9716)) * lfs(C + 1816));  // f1

  // The direction, normalised (the bias keeps a zero-length segment finite).
  const double dx = single(bx - ax), dy = single(by - ay);
  const double length2 = (dy * dy + dx * dx) + lfd(C + 10080);
  const double r = (double)(float)(1.0 / __builtin_sqrt(length2));
  const double nx = r * dx, ny = r * dy;
  const double k1 = lfd(C + 3200), k2 = lfd(C + 3376);
  const double nx_k1 = nx * k1;   // f30
  const double a = width * ny;    // f3: across, x
  const double b = width * nx_k1; // f27: across, y
  const double p = nx * k2;       // f29: along, x
  const double q = ny * k2;       // f28: along, y
  const double wn = width * nx;   // f28 later: the caps' length along

  auto vertex = [&](u32 out, double x, double y, double u, double w) {
    stfs(out + 0, x);
    stfs(out + 4, y);
    stfs(out + 8, z);
    stfs(out + 12, u);
    stfs(out + 16, red);
    stfs(out + 20, green);
    stfs(out + 24, blue);
    stfs(out + 28, w);
  };

  // The thin quad, w = the line's alpha.
  vertex(quad + 0, (ax - a) - p, (ay - b) - q, u0, alpha);
  vertex(quad + 32, (ax + a) - p, (ay + b) - q, one, alpha);
  vertex(quad + 64, (bx + p) + a, (by + q) + b, one, alpha);
  vertex(quad + 96, (bx - a) + p, (by - b) + q, u0, alpha);

  // The glow outline: the sides at w = 1, the caps' tips at w = 0.
  vertex(outline + 0, ax - a, ay - b, u0, one);
  vertex(outline + 32, ax + a, ay + b, one, one);
  vertex(outline + 64, (ax + a) - wn, (ay + b) - a, one, zero);
  vertex(outline + 96, (ax - a) - wn, (ay - b) - a, u0, zero);
  vertex(outline + 128, bx - a, by - b, u0, one);
  vertex(outline + 160, bx + a, by + b, one, one);
  vertex(outline + 192, (ny + nx) * width + bx, (nx_k1 + ny) * width + by, one, zero);
  vertex(outline + 224, width * nx + (bx - a), (by - b) + a, u0, zero);
}

// The 3D glowing-line builder (guest 0x8205EB58): the outlines of the game's
// objects (guest 0x82061DA0 transforms an object's points and joins them with
// it). r3, r4 the segment's ends (x, y, z), r5 the colour (r, g, b, a), f1 the
// length of the glow caps along the segment. Like sub_8205FBA8, a thin quad
// into one stream and an eight-vertex outline into another, the width at
// right angles in the xy plane, the caps along the segment in 3D; the colour
// is premultiplied by its alpha.
//
// The work is line3d below, shared with the object outline (which calls it
// directly, its ends and colour in registers). Its twelve vertices come from
// outline_build, in host order.
//
// Deferred outlines: in a crowd the outline streams take about 23,000
// segments a drawn frame, 12 vertices (384 bytes) each, which were written to
// the game's chunks and then read back, byte-swapped and copied by the
// renderer (two passes over 9 MB of memory a frame on a machine whose memory
// is shared with the GPU). The game keeps every chunk until it draws its
// streams at the end of the frame, so a host copy of each chunk would not fit
// in 64 MB. Instead, a chunk the native code fills from its start keeps only
// the segments' inputs (Segment, 56 bytes each, in host memory): the guest
// chunk gets a tag in the first 16 bytes of its first vertex (inside the
// native code's own vertices) and nothing else, and when the renderer draws
// from a tagged chunk (src/nv2a.cpp) gw_outline_vertices builds the vertices
// from the segments straight into the GPU's vertex memory, with the same
// arithmetic as before. A chunk stops being deferred as soon as other code
// takes vertices in it (the rest is written to the guest; the deferred part
// is the chunk's first vertices, all the native code's), and when the
// segments or chunks run out for a frame the vertices go to the guest as the
// original. The segments and chunks are kept for one drawn frame: the
// renderer runs on this thread, and a frame's draws have all been run by the
// time the next drawn frame builds its outlines.
namespace {

// One vertex, 8 words in host order.
struct Vertex {
  u32 w[8];
};
inline u32 host_float(double d) {
  const float f = (float)d;
  u32 v;
  memcpy(&v, &f, 4);
  return v;
}

// Writes n vertices big-endian to guest memory at dst.
inline void put_guest(u8* dst, const Vertex* v, u32 n) {
  Vertex t[8];
  for (u32 i = 0; i < n; ++i)
    for (u32 k = 0; k < 8; ++k) t[i].w[k] = __builtin_bswap32(v[i].w[k]);
  if (((uintptr_t)dst & 15) == 0) {
    for (u32 i = 0; i < n; ++i) {
      _mm_stream_ps((float*)(dst + i * 32), _mm_loadu_ps((const float*)&t[i].w[0]));
      _mm_stream_ps((float*)(dst + i * 32 + 16), _mm_loadu_ps((const float*)&t[i].w[4]));
    }
  } else {
    memcpy(dst, t, n * 32);
  }
}

// A segment's inputs: its ends, colour (r, g, b, a), the line's half width
// (K + 7572 + 32 when it was built) and the caps' length.
struct Segment {
  float p[3], q[3], colour[4], w;
  double cap;
};

// The twelve vertices of a segment: 0-3 the thin quad, 4-11 the outline.
void outline_build(const Segment& s, Vertex* v) {
  const float* colour = s.colour;
  const double alpha = colour[3];
  const double colour_scale = lfs(C + 10024);
  const u32 red = host_float(single(single(alpha * colour[0]) * colour_scale));
  const u32 green = host_float(single(single((double)colour[1] * alpha) * colour_scale));
  const u32 blue = host_float(single(single((double)colour[2] * alpha) * colour_scale));
  const double px = s.p[0], py = s.p[1], pz = s.p[2];
  const double qx = s.q[0], qy = s.q[1], qz = s.q[2];
  const double w = s.w, cap = s.cap;
  const u32 u0 = host_float(lfs(C + 3208)), one = host_float(lfs(C + 1732)), zero = host_float(lfs(C + 1740));
  const double dx = single(qx - px), dy = single(qy - py), dz = single(qz - pz);

  // 1 / length in the plane and in 3D (the bias keeps zero finite).
  const double bias = lfd(C + 10080);
  const double plane2 = (dx * dx + dy * dy) + bias;
  const double space2 = ((dz * dz + dx * dx) + dy * dy) + bias;
  const double r_plane = (double)(float)(1.0 / __builtin_sqrt(plane2));
  const double r_space = (double)(float)(1.0 / __builtin_sqrt(space2));
  const double along = r_space * cap;
  const double n = r_plane * dy;     // across, x (f9)
  const double m = -(r_plane * dx);  // across, y (f8)
  const double tx = along * dx, ty = along * dy, tz = along * dz;  // the caps (f31, f30, f29)

  auto fma = [](double a, double b, double c) { return a * b + c; };     // fmadd
  auto nms = [](double a, double b, double c) { return -(a * b - c); };  // fnmsub
  auto vertex = [&](Vertex& out, double x, double y, double z, u32 u, u32 vv) {
    out.w[0] = host_float(x);
    out.w[1] = host_float(y);
    out.w[2] = host_float(z);
    out.w[3] = u;
    out.w[4] = red;
    out.w[5] = green;
    out.w[6] = blue;
    out.w[7] = vv;
  };
  // The corners shared by the quad and the outline.
  const double ax = nms(w, n, px), ay = nms(w, m, py);  // start, one side
  const double bx = fma(w, n, px), by = fma(w, m, py);  // start, other side
  const double cx = fma(w, n, qx), cy = fma(w, m, qy);  // end, other side
  const double ex = nms(w, n, qx), ey = nms(w, m, qy);  // end, one side
  vertex(v[0], ax, ay, pz, u0, one);
  vertex(v[1], bx, by, pz, one, one);
  vertex(v[2], cx, cy, qz, one, one);
  vertex(v[3], ex, ey, qz, u0, one);
  vertex(v[4], ax, ay, pz, u0, one);
  vertex(v[5], bx, by, pz, one, one);
  vertex(v[6], nms(w, tx, bx), nms(w, ty, by), nms(w, tz, pz), one, zero);
  vertex(v[7], nms(w, tx, ax), nms(w, ty, ay), nms(w, tz, pz), u0, zero);
  vertex(v[8], ex, ey, qz, u0, one);
  vertex(v[9], cx, cy, qz, one, one);
  vertex(v[10], fma(n + tx, w, qx), fma(m + ty, w, qy), fma(w, tz, qz), one, zero);
  vertex(v[11], fma(w, tx, ex), fma(w, ty, ey), fma(w, tz, qz), u0, zero);
}

// --- deferred chunks -----------------------------------------------------------------
constexpr u32 kTag0 = 0x7FA5A5A5, kTag1 = 0x47573258;  // a signalling NaN, 'GW2X'
constexpr u32 kMaxSegments = 49152;                     // 2.6 MiB at most
constexpr u32 kKeepFree = 6 << 20;                      // memory left to the game
constexpr u32 kMaxChunks = 256;

struct Chunk {
  u32 guest;    // the chunk (its first vertex tagged)
  u32 id;       // the tag's serial
  u32 first;    // its first segment
  u32 verts;    // vertices deferred, from the chunk's start
  u32 per;      // vertices a segment: 4 (quads) or 8 (outlines)
  u32 corner;   // the segment's first vertex of this stream (0 or 4)
};
Segment* g_segments;
u32 g_segment_room, g_segment_count;
bool g_segments_refused;
Chunk g_chunks[kMaxChunks];
u32 g_chunk_count, g_serial;
u32 g_frame = ~0u;

// A new drawn frame: last frame's segments and chunks are done with.
inline void frame_check() {
  const u32 frame = gw_drawn_index();
  if (frame != g_frame) {
    g_frame = frame;
    g_segment_count = 0;
    g_chunk_count = 0;
  }
}

// Room for one more segment (growing the store while memory allows).
bool segment_room() {
#ifndef NXDK
  return false;  // the PC check (tools/native_check) compares the guest's vertices
#else
  if (g_segment_count < g_segment_room) return true;
  if (g_segments_refused || g_segment_room >= kMaxSegments) return false;
  const u32 want = g_segment_room ? g_segment_room * 2 : 8192;
  MM_STATISTICS stats;
  stats.Length = sizeof(stats);
  MmQueryStatistics(&stats);
  const u32 more = (want - g_segment_room) * sizeof(Segment);
  if (stats.AvailablePages * 4096 < kKeepFree + more) {
    g_segments_refused = true;
    gw_log("outlines: %u segments deferred a frame at most (%u KiB free)\n", g_segment_room,
           (u32)(stats.AvailablePages * 4));
    return false;
  }
  Segment* grown = (Segment*)realloc(g_segments, want * sizeof(Segment));
  if (!grown) {
    g_segments_refused = true;
    gw_log("outlines: no memory for %u deferred segments\n", want);
    return false;
  }
  g_segments = grown;
  g_segment_room = want;
  gw_log("outlines: room for %u deferred segments a frame (%u KiB free before)\n", want,
         (u32)(stats.AvailablePages * 4));
  return true;
#endif
}

// A stream's chunk as the native code is filling it.
struct Stream {
  u32 guest;      // the stream object (guest 0x82061750's: +2052 the chunk, +2056 vertices used)
  u32 per, corner;
  Chunk* chunk;   // deferred while not null
};
Stream g_quads{K + 12424, 4, 0, nullptr}, g_outlines{K + 14536, 8, 4, nullptr};

// Room for a segment's vertices in a stream (guest 0x82061750's: the usual
// case here, a full chunk to the game's code, which locks the next one).
// Returns where they go in the guest chunk, and with *defer whether they can
// be deferred (the chunk is the native code's from its start, and a chunk
// record was free).
u32 stream_room(PPCContext& ctx, uint8_t* base, Stream& s, bool* defer) {
  const u32 count = s.per;
  u32 used = gw_load32(s.guest + 2056);
  u32 at;
  if ((int)(used + count) <= 4096) {
    gw_store32(s.guest + 2056, used + count);
    at = gw_load32(s.guest + 2052) + (used << 5);
  } else {
    ctx.r1.u64 = ctx.r1.u32 - 192;  // the original's frame
    ctx.r3.u64 = s.guest;
    ctx.r4.u64 = count;
    sub_82061750(ctx, base);
    ctx.r1.u64 = ctx.r1.u32 + 192;
    at = ctx.r3.u32;
    used = (at - gw_load32(s.guest + 2052)) >> 5;
  }
  const u32 chunk = gw_load32(s.guest + 2052);
  if (used == 0) {
    s.chunk = nullptr;
    if (g_chunk_count < kMaxChunks && segment_room()) {
      Chunk& c = g_chunks[g_chunk_count++];
      c = {chunk, ++g_serial, g_segment_count, 0, s.per, s.corner};
      s.chunk = &c;
    }
  }
  *defer = s.chunk && s.chunk->guest == chunk && s.chunk->verts == used;
  if (!*defer) s.chunk = nullptr;  // other code wrote here: the rest of the chunk to the guest
  return at;
}

// The line from p to q (x, y, z), colour (r, g, b, a), cap length.
void line3d(PPCContext& ctx, uint8_t* base, const float* p, const float* q, const float* colour, double cap) {
  frame_check();
  bool defer_quad, defer_outline;
  const u32 quad = stream_room(ctx, base, g_quads, &defer_quad);
  const u32 outline = stream_room(ctx, base, g_outlines, &defer_outline);
  const Segment s = {{p[0], p[1], p[2]}, {q[0], q[1], q[2]}, {colour[0], colour[1], colour[2], colour[3]},
                     lfs(K + 7572 + 32), cap};
  if ((defer_quad || defer_outline) && !segment_room()) {
    // Out of segments: this one and the rest of both chunks to the guest.
    defer_quad = defer_outline = false;
    g_quads.chunk = g_outlines.chunk = nullptr;
  }
  if (defer_quad || defer_outline) {
    const u32 index = g_segment_count++;
    g_segments[index] = s;
    Stream* const streams[2] = {defer_quad ? &g_quads : nullptr, defer_outline ? &g_outlines : nullptr};
    const u32 at[2] = {quad, outline};
    for (int k = 0; k < 2; ++k) {
      Stream* st = streams[k];
      if (!st) continue;
      Chunk& c = *st->chunk;
      if (c.verts == 0) {  // its first: the tag (inside this segment's own vertices)
        gw_store32(at[k], kTag0);
        gw_store32(at[k] + 4, kTag1);
        gw_store32(at[k] + 8, c.id);
        gw_store32(at[k] + 12, ~c.id);
      }
      c.verts += st->per;
    }
    if (defer_quad && defer_outline) return;
  }
  Vertex v[12];
  outline_build(s, v);
  if (!defer_quad) put_guest(gw_host(quad), v, 4);
  if (!defer_outline) put_guest(gw_host(outline), v + 4, 8);
}

}  // namespace

// The renderer's side (src/nv2a.cpp): vertices [first, first + count) of the
// chunk at guest, built from its deferred segments into out (host order, 32
// bytes each). Returns how many of them were deferred (from first on; the
// rest are in the guest chunk); 0 if the chunk is not a deferred one.
namespace {
// The renderer's version of outline_build: the same corners in single
// precision with SSE (the vertices only feed the GPU, nothing in the game
// reads them back; the differences are below a millionth of a unit), and
// only the corners the stream wants: 'corner' 0 the quad's four, 4 the
// outline's eight. outline_build's double-precision arithmetic went through
// the Pentium III's x87 unit and every segment was built twice (once for
// each stream): about a tenth of the main thread in a crowd.
struct FastConstants {
  float colour_scale, u0, one, zero, bias;
};
FastConstants fast_constants() {
  return {lfs(C + 10024), lfs(C + 3208), lfs(C + 1732), lfs(C + 1740), (float)lfd(C + 10080)};
}
inline void put(u8*& out, float x, float y, float z, float u, __m128 rgb_v) {
  _mm_storeu_ps((float*)out, _mm_setr_ps(x, y, z, u));
  _mm_storeu_ps((float*)(out + 16), rgb_v);
  out += 32;
}
void fast_build(const FastConstants& k, const Segment& s, u32 corner, u32 from, u32 to, u8*& out) {
  const float alpha = s.colour[3];
  const float red = alpha * s.colour[0] * k.colour_scale, green = s.colour[1] * alpha * k.colour_scale;
  const float blue = s.colour[2] * alpha * k.colour_scale;
  const float px = s.p[0], py = s.p[1], pz = s.p[2], qx = s.q[0], qy = s.q[1], qz = s.q[2];
  const float w = s.w, cap = (float)s.cap;
  const float dx = qx - px, dy = qy - py, dz = qz - pz;
  const __m128 sums = _mm_setr_ps((dx * dx + dy * dy) + k.bias, ((dz * dz + dx * dx) + dy * dy) + k.bias, 1, 1);
  const __m128 r = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(sums));
  float rr[4];
  _mm_storeu_ps(rr, r);
  const float r_plane = rr[0], r_space = rr[1];
  const float along = r_space * cap;
  const float n = r_plane * dy, m = -(r_plane * dx);
  const float tx = along * dx, ty = along * dy, tz = along * dz;
  const float wn = w * n, wm = w * m, wtx = w * tx, wty = w * ty, wtz = w * tz;
  const float ax = px - wn, ay = py - wm, bx = px + wn, by = py + wm;
  const float cx = qx + wn, cy = qy + wm, ex = qx - wn, ey = qy - wm;
  const __m128 lit = _mm_setr_ps(red, green, blue, k.one), dark = _mm_setr_ps(red, green, blue, k.zero);
  for (u32 v = corner + from; v < corner + to; ++v) {
    switch (v) {
      case 0: case 4: put(out, ax, ay, pz, k.u0, lit); break;
      case 1: case 5: put(out, bx, by, pz, k.one, lit); break;
      case 2: case 9: put(out, cx, cy, qz, k.one, lit); break;
      case 3: case 8: put(out, ex, ey, qz, k.u0, lit); break;
      case 6: put(out, bx - wtx, by - wty, pz - wtz, k.one, dark); break;
      case 7: put(out, ax - wtx, ay - wty, pz - wtz, k.u0, dark); break;
      case 10: put(out, (n + tx) * w + qx, (m + ty) * w + qy, wtz + qz, k.one, dark); break;
      default: put(out, ex + wtx, ey + wty, qz + wtz, k.u0, dark); break;
    }
  }
}
}  // namespace

u32 gw_outline_vertices(u32 guest, u32 first, u32 count, u8* out) {
  if (gw_drawn_index() != g_frame) return 0;
  for (u32 i = 0; i < g_chunk_count; ++i) {
    const Chunk& c = g_chunks[i];
    if (c.guest != guest || first >= c.verts) continue;
    if (gw_load32(guest) != kTag0 || gw_load32(guest + 4) != kTag1 || gw_load32(guest + 8) != c.id ||
        gw_load32(guest + 12) != ~c.id)
      continue;  // rewritten by other code since
    const u32 end = first + count < c.verts ? first + count : c.verts;
    const FastConstants k = fast_constants();
    for (u32 n = first; n < end;) {
      const u32 seg = n / c.per, at = n % c.per;
      const u32 upto = end - (n - at) < c.per ? end - (n - at) : c.per;  // this segment's corners [at, upto)
      if (seg + 2 < (c.verts + c.per - 1) / c.per) _mm_prefetch((const char*)&g_segments[c.first + seg + 2], _MM_HINT_T0);
      fast_build(k, g_segments[c.first + seg], c.corner, at, upto, out);
      n += upto - at;
    }
    return end - first;
  }
  return 0;
}

// Test only (make OUTLINE_CHECK=y): outline segments with an end far
// outside the arena (beyond 1,500 units in any axis, or not a number),
// logged with who drew them (the caller's return address) and, for an
// object's outline, its matrix and model.
#ifdef GW_OUTLINE_CHECK
namespace {
u32 g_outline_wild, g_outline_logged;
void outline_check(const char* who, u32 lr, const float* p, const float* q, const float* mtx, u32 model, u32 point) {
  bool wild = false;
  for (int c = 0; c < 3; ++c) wild |= !(p[c] > -1500.0f && p[c] < 1500.0f) || !(q[c] > -1500.0f && q[c] < 1500.0f);
  // (and out of the playing plane: depth beyond 100 either way)
  wild |= !(p[2] > -100.0f && p[2] < 100.0f) || !(q[2] > -100.0f && q[2] < 100.0f);
  if (!wild) return;
  ++g_outline_wild;
  if (g_outline_logged >= 40 && g_outline_wild % 5000 != 0) return;
  ++g_outline_logged;
  gw_log("outline: wild segment %u from %s (caller %08X, model %08X, point %08X): (%d %d %d) - (%d %d %d)\n",
         g_outline_wild, who, lr, model, point, (int)p[0], (int)p[1], (int)p[2], (int)q[0], (int)q[1], (int)q[2]);
  if (mtx)
    gw_log("  matrix x1000: %d %d %d %d / %d %d %d %d / %d %d %d %d / %d %d %d %d\n", (int)(mtx[0] * 1000),
           (int)(mtx[1] * 1000), (int)(mtx[2] * 1000), (int)(mtx[3] * 1000), (int)(mtx[4] * 1000), (int)(mtx[5] * 1000),
           (int)(mtx[6] * 1000), (int)(mtx[7] * 1000), (int)(mtx[8] * 1000), (int)(mtx[9] * 1000),
           (int)(mtx[10] * 1000), (int)(mtx[11] * 1000), (int)(mtx[12] * 1000), (int)(mtx[13] * 1000),
           (int)(mtx[14] * 1000), (int)(mtx[15] * 1000));
}
}  // namespace
#define OUTLINE_CHECK(...) outline_check(__VA_ARGS__)
#else
#define OUTLINE_CHECK(...)
#endif

extern "C" void sub_8205EB58(PPCContext& ctx, uint8_t* base) {
  const u32 from = ctx.r3.u32, to = ctx.r4.u32, colour = ctx.r5.u32;
  const double cap = ctx.f1.f64;
  const float p[3] = {lfs(from), lfs(from + 4), lfs(from + 8)};
  const float q[3] = {lfs(to), lfs(to + 4), lfs(to + 8)};
  const float c[4] = {lfs(colour), lfs(colour + 4), lfs(colour + 8), lfs(colour + 12)};
  OUTLINE_CHECK("a line", ctx.lr, p, q, nullptr, 0, from);
  line3d(ctx, base, p, q, c, cap);
  _mm_sfence();
}

// An object's outline (guest 0x82061DA0): r3 the object's matrix, r4 its
// model, r5 a colour (4 floats), r6 the part to draw. The model's mesh (+44):
// +0 parts (12 bytes: +0 segments, +4 count), +4 part count, +12 the line
// caps' length; each segment 48 bytes: +0 start, +16 end, +32 colour. Per
// segment the colour times the given one, both ends through the matrix (as
// sub_82030338: each product and sum rounded to single, in its order), then
// the 3D glowing line (line3d above), all in registers. The player's and the
// enemies' Draw call it for every part: with a crowd on screen it was the
// main thread's largest cost.
extern "C" void sub_82061DA0(PPCContext& ctx, uint8_t* base) {
  const u32 matrix = ctx.r3.u32, model = ctx.r4.u32, colour = ctx.r5.u32, part = ctx.r6.u32;
  if (!model) return;
  const u32 mesh = gw_load32(model + 44);
  if (!mesh || part >= gw_load32(mesh + 4)) return;
  const u32 entry = gw_load32(mesh + 0) + part * 12;
  if (!entry || gw_load32(entry + 4) == 0) return;
  const double cap = lfs(mesh + 12);
  float mtx[16];
  for (u32 i = 0; i < 16; ++i) mtx[i] = lfs(matrix + i * 4);
  const float tint[4] = {lfs(colour), lfs(colour + 4), lfs(colour + 8), lfs(colour + 12)};
  // x, y, z of v (x, y, z, w) times the matrix.
  auto transform = [&](u32 v, float* out) {
    const float v0 = lfs(v), v1 = lfs(v + 4), v2 = lfs(v + 8), v3 = lfs(v + 12);
    for (u32 c = 0; c < 3; ++c) {
      float s = v0 * mtx[c] + v1 * mtx[4 + c];
      s = s + v2 * mtx[8 + c];
      out[c] = s + v3 * mtx[12 + c];
    }
  };
  const u32 r1 = ctx.r1.u32;
  ctx.r1.u64 = r1 - 208;  // the original's frame (for a chunk change's call)
  const u32 segments = gw_load32(entry + 0), count = gw_load32(entry + 4);
  for (u32 k = 0; k < count; ++k) {
    const u32 segment = segments + k * 48;
    const float c[4] = {tint[0] * lfs(segment + 32), lfs(segment + 36) * tint[1], lfs(segment + 40) * tint[2],
                        lfs(segment + 44) * tint[3]};
    float p[3], q[3];
    transform(segment, p);
    transform(segment + 16, q);
    OUTLINE_CHECK("an object", (u32)ctx.lr, p, q, mtx, model, segment);
    line3d(ctx, base, p, q, c, cap);
  }
  ctx.r1.u64 = r1;
  _mm_sfence();
}
