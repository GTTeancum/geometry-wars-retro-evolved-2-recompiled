// Native replacements for the game's maths library routines.
//
// The game's sine (guest 0x8212E9B0) works in double precision: reduce by pi,
// a degree-17 odd polynomial, the sign from the parity and from x. As
// recompiled code on the Pentium III that is all x87 work plus memory round
// trips; the game only ever needs single precision from it. Here: the same
// reduction and a degree-11 polynomial in single precision (SSE), with the
// original's edge case (+-0 returned as is); arguments beyond +-2048, where a
// single-precision reduction loses accuracy, go to the original.
#include "runtime.h"

extern "C" void __imp__sub_8212E9B0(PPCContext& ctx, uint8_t* base);

// sin(x) in single precision for |x| < 2048 (also used by src/stars_native.cpp).
float gw_sine(float x) {
  const float ax = __builtin_fabsf(x);
  // Nearest multiple of pi, split in two parts so the reduction stays exact.
  const float k = (float)(int)(ax * 0.318309886f + 0.5f);
  float r = ax - k * 3.140625f;
  r -= k * 9.67653589793e-4f;
  const float r2 = r * r;
  float p = -2.50521084e-8f;
  p = p * r2 + 2.75573192e-6f;
  p = p * r2 - 1.98412698e-4f;
  p = p * r2 + 8.33333333e-3f;
  p = p * r2 - 1.66666667e-1f;
  float s = r + r * r2 * p;
  if ((int)k & 1) s = -s;
  return x < 0 ? -s : s;
}

namespace {

// e^x in single precision for |x| < 80: x = k ln 2 + r, e^r from a degree-6
// polynomial, times 2^k through the exponent bits.
float exponential(float x) {
  const float k = __builtin_floorf(x * 1.44269504f + 0.5f);
  float r = x - k * 0.693359375f;
  r -= k * -2.12194440e-4f;
  float p = 1.0f / 720;
  p = p * r + 1.0f / 120;
  p = p * r + 1.0f / 24;
  p = p * r + 1.0f / 6;
  p = p * r + 0.5f;
  p = p * r + 1.0f;
  p = p * r + 1.0f;
  const int e = (int)k + 127;
  u32 bits = (u32)e << 23;
  float scale;
  memcpy(&scale, &bits, 4);
  return p * scale;
}

}  // namespace

extern "C" void __imp__sub_821308B8(PPCContext& ctx, uint8_t* base);

// exp(f1) -> f1 (guest 0x821308B8); outside +-80 (near the float limits,
// infinities, NaN) the original's double-precision path.
extern "C" void sub_821308B8(PPCContext& ctx, uint8_t* base) {
  const double x = ctx.f1.f64;
  if (!(__builtin_fabs(x) < 80.0)) {
    __imp__sub_821308B8(ctx, base);
    return;
  }
  ctx.f1.f64 = (double)exponential((float)x);
}

// cos(f1) -> f1 (guest 0x8212EA80): the original reduces |x| by (k - 1/2) pi
// with k the nearest whole number to (|x| + pi/2) / pi, then the sine's
// polynomial, negated for odd k. The same here in single precision (the
// constant split so (k - 1/2) times its first part is exact below 2048);
// larger arguments, infinities and NaN go to the original. Every enemy's
// rotation matrix asks for one each frame (guest 0x82041600).
extern "C" void __imp__sub_8212EA80(PPCContext& ctx, uint8_t* base);
float gw_cosine(float x) {
  const float ax = __builtin_fabsf(x);
  const float k = (float)(int)((ax + 1.57079633f) * 0.318309886f + 0.5f);
  const float h = k - 0.5f;
  float r = ax - h * 3.140625f;
  r -= h * 9.67653589793e-4f;
  const float r2 = r * r;
  float p = -2.50521084e-8f;
  p = p * r2 + 2.75573192e-6f;
  p = p * r2 - 1.98412698e-4f;
  p = p * r2 + 8.33333333e-3f;
  p = p * r2 - 1.66666667e-1f;
  const float s = r + r * r2 * p;
  return ((int)k & 1) ? -s : s;
}
extern "C" void sub_8212EA80(PPCContext& ctx, uint8_t* base) {
  const double x = ctx.f1.f64;
  if (!(__builtin_fabs(x) < 2048.0)) {
    __imp__sub_8212EA80(ctx, base);
    return;
  }
  ctx.f1.f64 = (double)gw_cosine((float)x);
}

// sin(f1) -> f1 (guest 0x8212E9B0).
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base) {
  const double x = ctx.f1.f64;
  if (x == 0.0) return;  // +-0 comes back unchanged
  if (!(__builtin_fabs(x) < 2048.0)) {  // large, infinite or NaN: the original's path
    __imp__sub_8212E9B0(ctx, base);
    return;
  }
  ctx.f1.f64 = (double)gw_sine((float)x);
}

// Vector times matrix (guest 0x82030338): r3 out (4 floats), r4 the vector,
// r5 the 4x4 matrix (rows of 4 floats). out[c] = v0 m[0][c] + v1 m[1][c]
// + v2 m[2][c] + v3 m[3][c], each product and sum rounded to single in that
// order (fmuls, fadds). Returns r3. Used throughout the game's transforms.
namespace {
inline float load_float(u32 a) {
  const u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline void store_float(u32 a, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  gw_store32(a, v);
}
void vector_times_matrix(float out[4], u32 vector, u32 matrix) {
  const float v0 = load_float(vector), v1 = load_float(vector + 4), v2 = load_float(vector + 8);
  const float v3 = load_float(vector + 12);
  for (u32 c = 0; c < 4; ++c) {
    float s = v0 * load_float(matrix + c * 4) + v1 * load_float(matrix + 16 + c * 4);
    s = s + v2 * load_float(matrix + 32 + c * 4);
    out[c] = s + v3 * load_float(matrix + 48 + c * 4);
  }
}
}  // namespace

extern "C" void sub_82030338(PPCContext& ctx, uint8_t* base) {
  (void)base;
  float out[4];
  vector_times_matrix(out, ctx.r4.u32, ctx.r5.u32);
  for (u32 c = 0; c < 4; ++c) store_float(ctx.r3.u32 + c * 4, out[c]);
}

// Rotation about an axis (guest 0x82041600): r3 the 4x4 out, r4 the axis
// (x, y, z), f1 the angle. Identity first, then with c = cos, s = sin and
// t = 1 - c the nine entries, each multiply rounded to single and each
// multiply-add done in double then rounded (as the recompiled fmadds), the
// axis read afresh for each entry in the original's order (so even an axis
// inside the matrix gives the same result). Every enemy's drawing builds
// one each frame. Returns r3.
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82041600(PPCContext& ctx, uint8_t* base) {
  const u32 out = ctx.r3.u32, axis = ctx.r4.u32;
  const double angle = ctx.f1.f64;
  const float one = load_float(0x82000000 + 1732), zero = load_float(0x82000000 + 1740);
  for (u32 i = 0; i < 16; ++i) store_float(out + i * 4, (i % 5 == 0) ? one : zero);
  ctx.f1.f64 = angle;
  sub_8212EA80(ctx, base);
  const double c = (double)(float)ctx.f1.f64;
  ctx.f1.f64 = angle;
  sub_8212E9B0(ctx, base);
  const double s = (double)(float)ctx.f1.f64;
  const double t = (double)(float)((double)one - c);
  auto L = [&](u32 k) { return (double)load_float(axis + k * 4); };
  auto mul = [](double a, double b) { return (double)(float)(a * b); };
  auto madd = [](double a, double b, double c2) { return (double)(float)(a * b + c2); };
  store_float(out + 0, (float)madd(mul(L(0), L(0)), t, c));
  store_float(out + 4, (float)madd(mul(t, L(0)), L(1), mul(L(2), s)));
  store_float(out + 8, (float)madd(mul(L(2), t), L(0), -mul(s, L(1))));
  store_float(out + 16, (float)madd(mul(t, L(0)), L(1), -mul(L(2), s)));
  store_float(out + 20, (float)madd(mul(L(1), L(1)), t, c));
  store_float(out + 24, (float)madd(mul(L(2), t), L(1), mul(L(0), s)));
  store_float(out + 32, (float)madd(mul(L(2), t), L(0), mul(s, L(1))));
  store_float(out + 36, (float)madd(mul(L(2), t), L(1), -mul(L(0), s)));
  store_float(out + 40, (float)madd(mul(L(2), L(2)), t, c));
  ctx.r3.u64 = out;
}

// Matrix times matrix (guest 0x82041CA0): r3 out, r4 a, r5 b, out = a b, row
// by row through a temporary (out may be a or b). Returns r3.
extern "C" void sub_82041CA0(PPCContext& ctx, uint8_t* base) {
  (void)base;
  float out[4][4];
  for (u32 row = 0; row < 4; ++row) vector_times_matrix(out[row], ctx.r4.u32 + row * 16, ctx.r5.u32);
  for (u32 row = 0; row < 4; ++row) {
    for (u32 c = 0; c < 4; ++c) store_float(ctx.r3.u32 + row * 16 + c * 4, out[row][c]);
  }
}
