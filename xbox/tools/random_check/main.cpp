// PC check of the native random number generator (src/random_native.cpp)
// against the recompiled originals (0x82030058, 0x82030220, 0x82030430,
// 0x8203B8B0), extracted by tools/random_check.cmd. Random states (and
// indices, so the regeneration runs often), long runs of every function,
// mixed: the results, the vectors written and the whole state compared.
// Built 32-bit so guest addresses wrap as on the Xbox.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "runtime.h"

#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) extern "C" void original_##name(PPCContext& ctx, uint8_t* base)
#define sub_82030058 original_sub_82030058
extern "C" void original_sub_82030058(PPCContext& ctx, uint8_t* base);
#include "original.inc"
#undef sub_82030058

void gw_log(const char*, ...) {}
// The native ones (src/random_native.cpp).
extern "C" void sub_82030058(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030220(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030430(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8203B8B0(PPCContext& ctx, uint8_t* base);

namespace {
constexpr u32 kStack = 0x70100000, kState = 0x70200000, kOut = 0x70210000;

bool map(u32 guest, u32 size) {
  size += guest & 0xFFFF;
  guest &= ~0xFFFFu;
  u8* host = gw_host(guest);
  return VirtualAlloc(host, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == host;
}
float as_float(u32 a) {
  const u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
}  // namespace

int main() {
  if (!map(0x82000000, 0x220000) || !map(kStack - 0x10000, 0x20000) || !map(kState, 0x20000)) {
    printf("could not map\n");
    return 1;
  }
  FILE* f = fopen("game\\analysis\\default.image.bin", "rb");
  if (!f) {
    printf("game\\analysis\\default.image.bin missing\n");
    return 1;
  }
  fread(gw_host(0x82000000), 1, 0x220000, f);
  fclose(f);

  int bad = 0, calls = 0;
  static u8 state0[2500], want_state[2500];
  for (int round = 0; round < 200; ++round) {
    srand(round + 1);
    for (u32 w = 0; w < 624; ++w) gw_store32(kState + w * 4, (u32)rand() << 17 ^ (u32)rand() << 2 ^ (u32)rand());
    gw_store32(kState + 2496, round % 3 == 0 ? 624 : (u32)(rand() % 625));
    memcpy(state0, gw_host(kState), 2500);
    // The same sequence of calls on both: kind and scale from a fixed list.
    static double want[4000], got[4000];
    static u32 want_vec[4000][2], got_vec[4000][2];
    for (int pass = 0; pass < 2; ++pass) {
      memcpy(gw_host(kState), state0, 2500);
      srand(1000 + round);
      for (int n = 0; n < 2000; ++n) {
        const int kind = rand() % 4;
        const double scale = (rand() % 2000 - 1000) * 0.01;
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        double r = 0;
        u32 v[2] = {0, 0};
        if (pass == 0) {
          switch (kind) {
            case 0: ctx.r3.u64 = kState; original_sub_82030058(ctx, gw_host(0)); r = ctx.r3.u32; break;
            case 1: ctx.r3.u64 = kState; ctx.f1.f64 = scale; original_sub_82030220(ctx, gw_host(0)); r = ctx.f1.f64; break;
            case 2: ctx.r3.u64 = kState; ctx.f1.f64 = scale; original_sub_82030430(ctx, gw_host(0)); r = ctx.f1.f64; break;
            default:
              ctx.r3.u64 = kOut; ctx.r4.u64 = kState; ctx.f1.f64 = scale;
              original_sub_8203B8B0(ctx, gw_host(0));
              v[0] = gw_load32(kOut); v[1] = gw_load32(kOut + 4); r = ctx.r3.u32;
          }
        } else {
          // The native entry points as the game calls them.
          switch (kind) {
            case 0: ctx.r3.u64 = kState; sub_82030058(ctx, gw_host(0)); r = ctx.r3.u32; break;
            case 1: ctx.r3.u64 = kState; ctx.f1.f64 = scale; sub_82030220(ctx, gw_host(0)); r = ctx.f1.f64; break;
            case 2: ctx.r3.u64 = kState; ctx.f1.f64 = scale; sub_82030430(ctx, gw_host(0)); r = ctx.f1.f64; break;
            default:
              ctx.r3.u64 = kOut; ctx.r4.u64 = kState; ctx.f1.f64 = scale;
              sub_8203B8B0(ctx, gw_host(0));
              v[0] = gw_load32(kOut); v[1] = gw_load32(kOut + 4); r = ctx.r3.u32;
          }
        }
        (pass == 0 ? want : got)[n] = r;
        memcpy((pass == 0 ? want_vec : got_vec)[n], v, 8);
        ++calls;
      }
      if (pass == 0) memcpy(want_state, gw_host(kState), 2500);
    }
    int differ = memcmp(want_state, gw_host(kState), 2500) != 0;
    for (int n = 0; n < 2000; ++n)
      differ += memcmp(&want[n], &got[n], 8) != 0 || memcmp(want_vec[n], got_vec[n], 8) != 0;
    if (differ && bad < 5) printf("  round %d: %d results differ\n", round, differ);
    bad += differ != 0;
  }
  printf("random numbers (0x82030058, 0x82030220, 0x82030430, 0x8203B8B0): %d of 200 runs differ (%d calls each side)\n",
         bad, calls / 2);
  return bad != 0;
}
