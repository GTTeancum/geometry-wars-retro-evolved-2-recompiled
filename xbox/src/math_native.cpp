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
