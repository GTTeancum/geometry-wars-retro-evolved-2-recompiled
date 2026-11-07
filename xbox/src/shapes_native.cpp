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
extern "C" void sub_8205EB58(PPCContext& ctx, uint8_t* base) {
  const u32 from = ctx.r3.u32, to = ctx.r4.u32, colour = ctx.r5.u32;
  const double cap = ctx.f1.f64;
  auto allocate = [&](u32 stream, u32 count) -> u32 {
    const u32 used = gw_load32(stream + 2056);
    if ((int)(used + count) <= 4096) {
      gw_store32(stream + 2056, used + count);
      return gw_load32(stream + 2052) + (used << 5);
    }
    ctx.r1.u64 = ctx.r1.u32 - 192;  // the original's frame
    ctx.r3.u64 = stream;
    ctx.r4.u64 = count;
    sub_82061750(ctx, base);
    ctx.r1.u64 = ctx.r1.u32 + 192;
    return ctx.r3.u32;
  };
  const u32 quad = allocate(K + 12424, 4);
  const u32 outline = allocate(K + 14536, 8);

  const double alpha = lfs(colour + 12);
  const double colour_scale = lfs(C + 10024);
  const double red = single(single(alpha * lfs(colour + 0)) * colour_scale);
  const double green = single(single(lfs(colour + 4) * alpha) * colour_scale);
  const double blue = single(single(lfs(colour + 8) * alpha) * colour_scale);
  const double px = lfs(from + 0), py = lfs(from + 4), pz = lfs(from + 8);
  const double qx = lfs(to + 0), qy = lfs(to + 4), qz = lfs(to + 8);
  const double w = lfs(K + 7572 + 32);
  const double u0 = lfs(C + 3208), one = lfs(C + 1732), zero = lfs(C + 1740);
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

  auto fma = [](double a, double b, double c) { return a * b + c; };      // fmadd
  auto nms = [](double a, double b, double c) { return -(a * b - c); };   // fnmsub
  auto vertex = [&](u32 out, double x, double y, double z, double u, double v) {
    stfs(out + 0, x);
    stfs(out + 4, y);
    stfs(out + 8, z);
    stfs(out + 12, u);
    stfs(out + 16, red);
    stfs(out + 20, green);
    stfs(out + 24, blue);
    stfs(out + 28, v);
  };

  vertex(quad + 0, nms(w, n, px), nms(w, m, py), pz, u0, one);
  vertex(quad + 32, fma(w, n, px), fma(w, m, py), pz, one, one);
  vertex(quad + 64, fma(w, n, qx), fma(w, m, qy), qz, one, one);
  vertex(quad + 96, nms(w, n, qx), nms(w, m, qy), qz, u0, one);

  vertex(outline + 0, nms(w, n, px), nms(w, m, py), pz, u0, one);
  vertex(outline + 32, fma(w, n, px), fma(w, m, py), pz, one, one);
  vertex(outline + 64, nms(w, tx, fma(w, n, px)), nms(w, ty, fma(w, m, py)), nms(w, tz, pz), one, zero);
  vertex(outline + 96, nms(w, tx, nms(w, n, px)), nms(w, ty, nms(w, m, py)), nms(w, tz, pz), u0, zero);
  vertex(outline + 128, nms(w, n, qx), nms(w, m, qy), qz, u0, one);
  vertex(outline + 160, fma(w, n, qx), fma(w, m, qy), qz, one, one);
  vertex(outline + 192, fma(n + tx, w, qx), fma(m + ty, w, qy), fma(w, tz, qz), one, zero);
  vertex(outline + 224, fma(w, tx, nms(w, n, qx)), fma(w, ty, nms(w, m, qy)), fma(w, tz, qz), u0, zero);
}
