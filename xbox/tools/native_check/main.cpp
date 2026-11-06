// PC check of the native grid update (src/grid_native.cpp) against the
// recompiled original (guest 0x8205B880, extracted from xbox/build/gen by
// tools/grid_check.cmd into build/grid_check/original.inc).
//
// The game image (game/analysis/default.image.bin) supplies the constants; the
// grid, the pushers, the spatial-hash cells and the output buffers are made up
// here, close to what a level holds. Both versions run all 88 rows on the same
// input and everything they write is compared.
//   grid_check.exe [seed]
// Built 32-bit so guest addresses wrap as on the Xbox (host = guest - 0x5E000000).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "runtime.h"

// The recompiled original, renamed so it can sit beside the native one.
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) extern "C" void original_##name(PPCContext& ctx, uint8_t* base)
#include "original.inc"

extern "C" void sub_8205B880(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8212E9B0(PPCContext& ctx, uint8_t* base) { original_sub_8212E9B0(ctx, base); }

std::atomic<i32> gw_global_lock_count{0};
namespace rex::thread {
void RecursiveLock::lock() {}
void RecursiveLock::unlock() {}
RecursiveLock& global_critical_region::mutex() {
  static RecursiveLock lock;
  return lock;
}
}  // namespace rex::thread

void gw_log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

void gw_fatal(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  exit(1);
}

jmp_buf* gw_jmp_buf_for(u32, bool) { return nullptr; }
namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t) { return nullptr; }
}  // namespace rex::runtime
GwDispatchEntry gw_dispatch_table[1u << kGwDispatchBits];

namespace {

constexpr u32 C = 0x82000000, D = 0x82180000, K = 0x821A0000;
constexpr u32 kRow = 133 * 40;

// Guest layout for the test.
constexpr u32 kStack = 0x70100000;     // r1
constexpr u32 kArgs = 0x70200000;      // job argument
constexpr u32 kCells = 0x70210000;     // cell table, objects
constexpr u32 kGrid = 0xC0010000;      // row 0 of the grid (one row above it)
constexpr u32 kLines = 0xC0200000;     // line vertices (rows start 14784 before)
constexpr u32 kDisplace = 0xC0600000;  // displacement vertices

float frand(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }
void sf(u32 a, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  gw_store32(a, v);
}
float lf(u32 a) {
  u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}

bool map(u32 guest, u32 size) {
  size += guest & 0xFFFF;
  guest &= ~0xFFFFu;
  u8* host = gw_host(guest);
  if (VirtualAlloc(host, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != host) {
    printf("could not map guest %08X+%X at host %p\n", guest, size, host);
    return false;
  }
  return true;
}

void setup(u32 seed) {
  srand(seed);
  // Runtime state the game keeps in its data section.
  const float half_w = 640, half_h = 360;
  sf(D + 9652, half_w);
  sf(D + 9656, half_h);
  gw_store32(D + 9660, seed & 1);         // read slot
  gw_store32(K + 5908, (seed & 1) ^ 1);   // write slot
  gw_store32(K + 5896, kGrid);
  gw_store32(K + 5888, kLines + 14784);
  gw_store32(K + 5884, kDisplace);
  gw_store32(K + 5892, 0xFFFFFFFF);  // row counter: the first job takes row 0
  sf(K + 1540, half_w);
  sf(K + 1544, half_h);
  sf(D + 9640, 16.0f / (2 * half_w));
  sf(D + 9644, 16.0f / (2 * half_h));
  gw_store32(K + 1556, 16);
  gw_store32(K + 1560, 16);
  for (int i = 0; i < 8; ++i) gw_store32(K + 5852 + i * 4, 0xFF000000u | (u32)rand() << 8 | (u32)i);
  sf(K + 6964, 1.5f);
  sf(K + 5900, 1.0f);
  sf(K + 7016, 0.5f);

  // Grid: rest on a lattice, both position slots near it, small velocities.
  for (u32 j = 0; j < 91; ++j) {
    for (u32 i = 0; i < 133; ++i) {
      u32 p = kGrid - kRow + j * kRow + i * 40;
      float rx = -half_w + i * (2 * half_w / 132), ry = -half_h + j * (2 * half_h / 90);
      sf(p + 0, frand(-2, 2));
      sf(p + 4, frand(-2, 2));
      for (u32 s = 0; s < 2; ++s) {
        sf(p + 8 + s * 8, rx + frand(-6, 6));
        sf(p + 12 + s * 8, ry + frand(-6, 6));
      }
      sf(p + 24, rx);
      sf(p + 28, ry);
      sf(p + 32, i / 132.0f);
      sf(p + 36, j / 90.0f);
    }
  }

  // Pushers (position, radius, strength) and the two extra pushers.
  gw_store32(kArgs + 0, 6);
  gw_store8(kArgs + 4, 1);
  gw_store8(kArgs + 5, 1);
  sf(kArgs + 16, frand(-300, 300));
  sf(kArgs + 20, frand(-200, 200));
  sf(kArgs + 24, frand(-300, 300));
  sf(kArgs + 28, frand(-200, 200));
  for (u32 n = 0; n < 6; ++n) {
    u32 q = kArgs + 32 + n * 16;
    sf(q + 0, frand(-600, 600));
    sf(q + 4, frand(-340, 340));
    sf(q + 8, frand(0.5f, 40));
    sf(q + 12, frand(-30, 30));
  }

  // Spatial hash: 16 x 16 cells, each with 0-3 objects (x, y, radius^2, strength).
  const u32 table = kCells + 0x100, objects = kCells + 0x1000;
  gw_store32(K + 7472 + 8, table);
  gw_store32(K + 7472 + 4, objects);
  u32 count = 0;
  for (u32 c = 0; c < 256; ++c) {
    gw_store32(table + c * 4, count);
    u32 n = rand() % 4;
    for (u32 k = 0; k < n; ++k, ++count) {
      u32 o = objects + count * 20 + 4;
      float cx = -half_w + ((c % 16) + 0.5f) * (2 * half_w / 16), cy = -half_h + ((c / 16) + 0.5f) * (2 * half_h / 16);
      sf(o + 0, cx + frand(-30, 30));
      sf(o + 4, cy + frand(-20, 20));
      sf(o + 8, frand(100, 4000));
      sf(o + 12, frand(-20, 20));
    }
  }
  gw_store32(table + 256 * 4, count);
}

struct Region {
  const char* name;
  u32 guest, size;
};
const Region kRegions[] = {
    {"points", kGrid - kRow, 91 * kRow},
    {"lines", kLines, 88 * 1052 * 28},
    {"displacements", kDisplace, 89 * 133 * 28},
};

int compare(const Region& r, const u8* want) {
  const u8* got = gw_host(r.guest);
  u32 bad = 0, differ = 0;
  float worst = 0;
  u32 worst_at = 0;
  for (u32 o = 0; o + 4 <= r.size; o += 4) {
    u32 a, b;
    memcpy(&a, want + o, 4);
    memcpy(&b, got + o, 4);
    if (a == b) continue;
    ++differ;
    a = __builtin_bswap32(a);
    b = __builtin_bswap32(b);
    float fa, fb;
    memcpy(&fa, &a, 4);
    memcpy(&fb, &b, 4);
    float err = fabsf(fa - fb) / (1.0f + fabsf(fa));
    if (!(err <= 2e-3f)) {
      if (bad < 5) printf("    %s +%X: original %g, native %g\n", r.name, o, fa, fb);
      ++bad;
    }
    if (!(err <= worst)) {
      worst = err;
      worst_at = o;
    }
  }
  printf("  %-13s %7u words, %7u differ, %5u beyond 0.2%% (worst %.2g at +%X)\n", r.name, r.size / 4, differ, bad,
         worst, worst_at);
  return (int)bad;
}

}  // namespace

LONG WINAPI report_fault(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD* r = e->ExceptionRecord;
  u32 host = (u32)r->ExceptionInformation[1];
  printf("fault at host code %p touching host %08X (guest %08X)\n", r->ExceptionAddress, host, host + 0x5E000000u);
  exit(2);
}

int main(int argc, char** argv) {
  SetUnhandledExceptionFilter(report_fault);
  u32 seed = argc > 1 ? (u32)atoi(argv[1]) : 1;
  if (!map(0x82000000, 0x220000) || !map(kStack - 0x10000, 0x20000) || !map(kArgs, 0x20000) ||
      !map(kGrid - kRow, 91 * kRow) || !map(kLines, 88 * 1052 * 28 + 0x1000) || !map(kDisplace, 89 * 133 * 28 + 0x1000)) {
    return 1;
  }
  FILE* f = fopen("..\\game\\analysis\\default.image.bin", "rb");
  if (!f) f = fopen("game\\analysis\\default.image.bin", "rb");
  if (!f) {
    printf("game\\analysis\\default.image.bin missing (xbox\\tools\\dump_image.cmd)\n");
    return 1;
  }
  fread(gw_host(0x82000000), 1, 0x220000, f);
  fclose(f);

  int bad = 0;
  for (u32 s = seed; s < seed + 4; ++s) {
    setup(s);
    // Snapshot the inputs, run the original, keep its output, restore, run native.
    u8* image = (u8*)malloc(0x220000);
    memcpy(image, gw_host(0x82000000), 0x220000);
    u8* before[3];
    u8* want[3];
    for (int r = 0; r < 3; ++r) {
      before[r] = (u8*)malloc(kRegions[r].size);
      want[r] = (u8*)malloc(kRegions[r].size);
      memcpy(before[r], gw_host(kRegions[r].guest), kRegions[r].size);
    }
    static PPCContext ctx;
    ctx = PPCContext{};
    ctx.r1.u64 = kStack;
    ctx.r3.u64 = kArgs;
    printf("original...\n");
    original_sub_8205B880(ctx, gw_host(0));
    printf("native...\n");
    for (int r = 0; r < 3; ++r) {
      memcpy(want[r], gw_host(kRegions[r].guest), kRegions[r].size);
      memcpy(gw_host(kRegions[r].guest), before[r], kRegions[r].size);
    }
    memcpy(gw_host(0x82000000), image, 0x220000);
    ctx = PPCContext{};
    ctx.r1.u64 = kStack;
    ctx.r3.u64 = kArgs;
    sub_8205B880(ctx, gw_host(0));
    printf("seed %u:\n", s);
    for (int r = 0; r < 3; ++r) {
      bad += compare(kRegions[r], want[r]);
      free(before[r]);
      free(want[r]);
    }
    free(image);
  }

  // Vector load/store byte reversal (prepare_generated.py) against SIMDe.
  {
    int differ = 0;
    for (int n = 0; n < 10000; ++n) {
      alignas(16) u8 src[16], want[16], got[16];
      for (u8& b : src) b = (u8)rand();
      simde_mm_store_si128((simde__m128i*)want, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)src),
                                                                       simde_mm_load_si128((simde__m128i*)VectorMaskL)));
      gw_copy_reversed(got, src);
      differ += memcmp(want, got, 16) != 0;
    }
    printf("vector byte reversal: %d of 10000 differ\n", differ);
    bad += differ;
  }

  // Truncating conversions (fctiwz/fctidz) against SIMDe.
  {
    int differ = 0;
    const double edges[] = {0.0, -0.0, 0.5, -0.5, 0.9999999999, -0.9999999999, 1.0, -1.0, 2.5, -2.5,
                            2147483646.9, 2147483647.0, -2147483648.0, -2147483648.5, -2147483649.0, -3e9,
                            4503599627370495.5, -4503599627370495.5, 1e17, -1e17, 123456789.99999};
    for (int n = 0; n < 400000; ++n) {
      double x;
      if (n < (int)(sizeof(edges) / sizeof(edges[0]))) x = edges[n];
      else if (n & 1) x = ((double)rand() * 65536.0 + rand()) / 1024.0 * (n & 2 ? 1 : -1);
      else x = (double)frand(-3e9f, 3e9f) + (double)rand() / RAND_MAX;
      if (x < 2147483647.0) {
        i32 want = simde_mm_cvttsd_si32(simde_mm_load_sd(&x)), got = gw_trunc_i32(x);
        if (want != got && differ++ < 5) printf("    trunc32(%.17g): SIMDe %d, native %d\n", x, want, got);
      }
      i64 want = simde_mm_cvttsd_si64(simde_mm_load_sd(&x)), got = gw_trunc_i64(x);
      if (want != got && differ++ < 5) printf("    trunc64(%.17g): SIMDe %lld, native %lld\n", x, want, got);
    }
    printf("truncating conversions: %d differ\n", differ);
    bad += differ;
  }

  // Sine (guest 0x8212E9B0): random arguments over the game's range, plus edges.
  {
    static PPCContext ctx;
    double worst = 0, worst_x = 0;
    const double edges[] = {0.0, -0.0, 1e-30, -1e-30, 1.5707963, 3.1415926, -3.1415926, 2047.9, 4096.0, 1e9};
    for (int n = 0; n < 200000 + 10; ++n) {
      double x = n < 10 ? edges[n] : (double)frand(-2100, 2100) * (n & 1 ? 1.0 : 1e-3);
      x = (double)(float)x;  // the game passes single-precision values
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      ctx.f1.f64 = x;
      original_sub_8212E9B0(ctx, gw_host(0));
      const double want = ctx.f1.f64;
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      ctx.f1.f64 = x;
      sub_8212E9B0(ctx, gw_host(0));
      const double got = ctx.f1.f64;
      const double err = want != want ? (got != got ? 0 : 1) : fabs(want - got);
      if (err > worst) {
        worst = err;
        worst_x = x;
      }
    }
    printf("sine: worst difference %.2g (at x = %g)\n", worst, worst_x);
    if (worst > 1e-5) ++bad;
  }

  // Speed on this PC (relative numbers only; the Xbox is far slower).
  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  double ms[2];
  for (int which = 0; which < 2; ++which) {
    QueryPerformanceCounter(&t0);
    for (int n = 0; n < 50; ++n) {
      gw_store32(K + 5892, 0xFFFFFFFF);
      static PPCContext ctx;
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      ctx.r3.u64 = kArgs;
      if (which == 0) original_sub_8205B880(ctx, gw_host(0));
      else sub_8205B880(ctx, gw_host(0));
    }
    QueryPerformanceCounter(&t1);
    ms[which] = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart / 50;
  }
  printf("one grid update: original %.3f ms, native %.3f ms (%.1fx)\n", ms[0], ms[1], ms[0] / ms[1]);
  printf(bad ? "MISMATCH\n" : "native grid update matches the original\n");
  return bad ? 1 : 0;
}
