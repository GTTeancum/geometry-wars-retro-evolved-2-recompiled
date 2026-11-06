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
#include "dxt.h"

// The recompiled original, renamed so it can sit beside the native one.
// What the extracted originals call by name.
extern "C" void sub_820A1800(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A1850(PPCContext& ctx, uint8_t* base);
extern "C" void sub_821303B8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A87B0(PPCContext& ctx, uint8_t* base);
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) extern "C" void original_##name(PPCContext& ctx, uint8_t* base)
#ifdef GW_NONVOLATILE_ORIGINAL
// Experiment: the originals with plain (non-volatile) guest loads and stores,
// to measure what volatile costs the recompiled code.
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (*(u8*)REX_HOST(x))
#define REX_LOAD_U16(x) __builtin_bswap16(*(u16*)REX_HOST(x))
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32*)REX_HOST(x))
#define REX_LOAD_U64(x) __builtin_bswap64(*(u64*)REX_HOST(x))
#define REX_STORE_U8(x, y) (*(u8*)REX_HOST(x) = (y))
#define REX_STORE_U16(x, y) (*(u16*)REX_HOST(x) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(u32*)REX_HOST(x) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(u64*)REX_HOST(x) = __builtin_bswap64(y))
#endif
#include "original.inc"

extern "C" void sub_8205B880(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82063448(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82075548(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A91A0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_821308B8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82096F30(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A6438(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A6518(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_821308B8(PPCContext& ctx, uint8_t* base) { original_sub_821308B8(ctx, base); }
// The constant upload's full-buffer helper: never reached (the test buffer has room).
extern "C" void sub_820A87B0(PPCContext&, uint8_t*) { gw_fatal("sub_820A87B0 reached"); }

// Stand-ins for what the starfield calls: the vertex buffer lock returns the
// buffer address plus kLockedOffset as its data; pow is the C library's.
constexpr u32 kLockedOffset = 0x100;
extern "C" void sub_820A1800(PPCContext& ctx, uint8_t*) {
  ctx.r3.u64 = ctx.r3.u32 + kLockedOffset;
}
extern "C" void sub_820A1850(PPCContext&, uint8_t*) {}
extern "C" void sub_821303B8(PPCContext& ctx, uint8_t*) { ctx.f1.f64 = pow(ctx.f1.f64, ctx.f2.f64); }
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
// No host mirror here: the native grid writes its lines into the guest
// buffer, where the comparison reads them.
u8* gw_grid_line_host(u32) { return nullptr; }
volatile u64 gw_part_cycles[kPartCount];
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
constexpr u32 kStarBuffer = 0xC0700000;  // three starfield vertex buffers

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
      !map(kGrid - kRow, 91 * kRow) || !map(kLines, 88 * 1052 * 28 + 0x1000) || !map(kDisplace, 89 * 133 * 28 + 0x1000) || !map(kStarBuffer, 3 * 0x30000)) {
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

  // Particle update (guest 0x82063448): random particles, wells and holes.
  {
    constexpr u32 kSystem = kArgs + 0x1000, kIn = kArgs + 0x2000, kOut = kArgs + 0x2100;
    constexpr u32 kStreak = kArgs + 0x2200, kGlowQuad = kArgs + 0x2300;
    int bad_words = 0;
    float worst = 0;
    for (int n = 0; n < 20000; ++n) {
      sf(0x821B0000 + 16032 + 32, 1.0f / 60);
      sf(0x821B0000 + 16120 + 32, frand(1, 20));
      sf(0x821B0000 + 16172 + 32, frand(0.5f, 4));
      const u32 system = kSystem + 40;
      const u32 wells = rand() % 4, holes = rand() % 3;
      gw_store32(system + 448, wells);
      gw_store32(system + 452, holes);
      for (u32 k = 0; k < 4; ++k) {
        for (u32 c = 0; c < 3; ++c) sf(system + k * 12 + c * 4, frand(-500, 500));
        for (u32 c = 0; c < 3; ++c) sf(system + 192 + k * 12 + c * 4, frand(-500, 500));
        sf(system + 384 + k * 4, frand(-3, 3));
      }
      for (u32 c = 0; c < 3; ++c) sf(kIn + c * 4, frand(-700, 700) * (c == 2 ? 0.01f : 1.0f));
      for (u32 c = 0; c < 3; ++c) sf(kIn + 12 + c * 4, frand(-3000, 3000) * (c == 2 ? 0.01f : 1.0f));
      for (u32 c = 0; c < 3; ++c) sf(kIn + 24 + c * 4, frand(0, 1));
      sf(kIn + 36, frand(0, 1));
      sf(kIn + 40, frand(0, 60));
      u8 want[3][128], got[3][128];
      for (int pass = 0; pass < 2; ++pass) {
        memset(gw_host(kOut), 0, 44);
        memset(gw_host(kStreak), 0, 128);
        memset(gw_host(kGlowQuad), 0, 112);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kSystem;
        ctx.r4.u64 = kIn;
        ctx.r5.u64 = kOut;
        ctx.r6.u64 = kStreak;
        ctx.r7.u64 = kGlowQuad;
        if (pass == 0) original_sub_82063448(ctx, gw_host(0));
        else sub_82063448(ctx, gw_host(0));
        u8(*dst)[128] = pass == 0 ? want : got;
        memcpy(dst[0], gw_host(kOut), 44);
        memcpy(dst[1], gw_host(kStreak), 128);
        memcpy(dst[2], gw_host(kGlowQuad), 112);
      }
      const u32 sizes[3] = {44, 128, 112};
      for (int r = 0; r < 3; ++r) {
        for (u32 o = 0; o < sizes[r]; o += 4) {
          u32 a, b;
          memcpy(&a, want[r] + o, 4);
          memcpy(&b, got[r] + o, 4);
          a = __builtin_bswap32(a);
          b = __builtin_bswap32(b);
          float fa, fb;
          memcpy(&fa, &a, 4);
          memcpy(&fb, &b, 4);
          const float err = fabsf(fa - fb) / (1.0f + fabsf(fa));
          if (!(err <= 1e-3f)) {
            if (bad_words++ < 6) printf("    particle %d %s +%u: original %g, native %g\n", n,
                                        r == 0 ? "state" : r == 1 ? "streak" : "glow", o, fa, fb);
          }
          if (err > worst) worst = err;
        }
      }
    }
    printf("particle update: %d values off by more than 0.1%% (worst %.2g)\n", bad_words, worst);
    bad += bad_words;
  }

  // Starfield (guest 0x82075548), with the vertex buffer lock and pow stubbed.
  {
    constexpr u32 kStars = 0x821B0000 + 18288;
    u8* stars_before = (u8*)malloc(6000 * 20);
    for (u32 n = 0; n < 6000; ++n) {
      const u32 s = kStars + n * 20;
      sf(s + 0, frand(-640, 640));
      sf(s + 4, frand(-360, 360));
      sf(s + 8, frand(0, 1));
      sf(s + 12, frand(-0.1f, 1));
      sf(s + 16, frand(-1, 1));
    }
    sf(0x821B0000 + 18264, 0.7f);
    sf(D + 9708, 1.25f);
    sf(D + 9880, 3.14159f);
    gw_store32(0x821B0000 + 18284, 1);
    for (u32 i = 0; i < 3; ++i) gw_store32(0x821B0000 + 18268 + i * 4, kStarBuffer + i * 0x30000);
    memcpy(stars_before, gw_host(kStars), 6000 * 20);
    u8* image_before = (u8*)malloc(0x220000);
    memcpy(image_before, gw_host(0x82000000), 0x220000);
    u8* want = (u8*)malloc(6000 * 24 + 6000 * 20);
    for (int pass = 0; pass < 2; ++pass) {
      memcpy(gw_host(0x82000000), image_before, 0x220000);
      static PPCContext ctx;
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      if (pass == 0) original_sub_82075548(ctx, gw_host(0));
      else sub_82075548(ctx, gw_host(0));
      const u32 buffer = kStarBuffer + 2 * 0x30000 + kLockedOffset;  // index 1 -> 2
      if (pass == 0) {
        memcpy(want, gw_host(buffer), 6000 * 24);
        memcpy(want + 6000 * 24, gw_host(kStars), 6000 * 20);
      } else {
        int differ = 0;
        float worst = 0;
        auto cmp = [&](const u8* a8, const u8* b8, u32 size) {
          for (u32 o = 0; o < size; o += 4) {
            u32 a, b;
            memcpy(&a, a8 + o, 4);
            memcpy(&b, b8 + o, 4);
            a = __builtin_bswap32(a);
            b = __builtin_bswap32(b);
            float fa, fb;
            memcpy(&fa, &a, 4);
            memcpy(&fb, &b, 4);
            const float err = fabsf(fa - fb) / (1.0f + fabsf(fa));
            if (!(err <= 1e-4f)) ++differ;
            if (err > worst) worst = err;
          }
        };
        cmp(want, gw_host(buffer), 6000 * 24);
        cmp(want + 6000 * 24, gw_host(kStars), 6000 * 20);
        printf("starfield: %d values off by more than 0.01%% (worst %.2g)\n", differ, worst);
        bad += differ;
      }
    }
    memcpy(gw_host(0x82000000), image_before, 0x220000);
    free(stars_before);
    free(image_before);
    free(want);
  }

  // Exponential (guest 0x821308B8): relative error over the game's range.
  {
    static PPCContext ctx;
    double worst = 0, worst_x = 0;
    for (int n = 0; n < 200000; ++n) {
      const double x = (double)(float)((double)frand(-90, 90) * (n & 1 ? 1.0 : 0.05));
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      ctx.f1.f64 = x;
      original_sub_821308B8(ctx, gw_host(0));
      const double want = ctx.f1.f64;
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      ctx.f1.f64 = x;
      sub_821308B8(ctx, gw_host(0));
      const double err = fabs(want - ctx.f1.f64) / (fabs(want) > 1e-30 ? fabs(want) : 1.0);
      if (err > worst) {
        worst = err;
        worst_x = x;
      }
    }
    printf("exp: worst relative difference %.2g (at x = %g)\n", worst, worst_x);
    if (worst > 1e-5) ++bad;
  }

  // Shader constant upload (guest 0x820A91A0): random dirty masks into a
  // command buffer with room (the full-buffer path calls into Direct3D).
  {
    constexpr u32 kDevice = kArgs + 0x8000, kBuffer = 0xC0740000, kSource = 0xC0780000;
    int differ = 0;
    for (int n = 0; n < 2000; ++n) {
      for (u32 i = 0; i < 0x4000; i += 4) gw_store32(kSource + i, (u32)rand() << 16 ^ (u32)rand());
      const u32 start = kBuffer + 4 * (u32)(rand() % 4);
      u64 mask = 0;
      for (int b = 0; b < 4; ++b) mask = mask << 16 ^ (u64)(rand() & 0xFFFF);
      if (n % 7 == 0) mask = ~0ull;
      const u32 reg = (u32)(rand() % 64);
      u8 want[0x5000];
      for (int pass = 0; pass < 2; ++pass) {
        memset(gw_host(kBuffer), 0xEE, 0x5000);
        gw_store32(kDevice + 48, start);
        gw_store32(kDevice + 52, kBuffer + 0x5000);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kDevice;
        ctx.r4.u64 = mask;
        ctx.r5.u64 = reg;
        ctx.r6.u64 = kSource;
        if (pass == 0) original_sub_820A91A0(ctx, gw_host(0));
        else sub_820A91A0(ctx, gw_host(0));
        if (pass == 0) {
          memcpy(want, gw_host(kBuffer), 0x5000);
          memcpy(want + 0x4FF8, gw_host(kDevice + 48), 4);  // compared separately below
        } else {
          u32 want_end;
          memcpy(&want_end, want + 0x4FF8, 4);
          if (memcmp(want, gw_host(kBuffer), 0x4FF8) != 0 || want_end != *(u32*)gw_host(kDevice + 48)) ++differ;
        }
      }
    }
    printf("shader constant upload: %d of 2000 differ\n", differ);
    if (differ) ++bad;
  }

  // Large-block copy (guest 0x82096F30): 128-aligned destination, any source
  // alignment, multiples of 128 bytes; the copy and the pointers it returns.
  {
    constexpr u32 kSrc = 0xC0700000, kDst = 0xC0760000;
    int differ = 0;
    for (int n = 0; n < 500; ++n) {
      for (u32 i = 0; i < 0x8000; i += 4) gw_store32(kSrc + i, (u32)rand() << 16 ^ (u32)rand());
      const u32 src = kSrc + (u32)(rand() % 256), dst = kDst + 128 * (u32)(rand() % 8);
      const u32 len = 128 * (u32)(1 + rand() % 40);
      u8 want[0x8000];
      u32 want_r3 = 0, want_r4 = 0;
      for (int pass = 0; pass < 2; ++pass) {
        memset(gw_host(kDst), 0xEE, 0x8000);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = dst;
        ctx.r4.u64 = src;
        ctx.r5.u64 = len;
        if (pass == 0) original_sub_82096F30(ctx, gw_host(0));
        else sub_82096F30(ctx, gw_host(0));
        if (pass == 0) {
          memcpy(want, gw_host(kDst), 0x8000);
          want_r3 = ctx.r3.u32;
          want_r4 = ctx.r4.u32;
        } else if (memcmp(want, gw_host(kDst), 0x8000) != 0 || want_r3 != ctx.r3.u32 || want_r4 != ctx.r4.u32) {
          ++differ;
        }
      }
    }
    printf("large-block copy: %d of 500 differ\n", differ);
    if (differ) ++bad;
  }

  // Shader constant shadows (guest 0x820A6438 vertex, 0x820A6518 pixel).
  {
    constexpr u32 kDevice = 0xC0700000, kSrc = 0xC0740000;
    int differ = 0;
    for (int n = 0; n < 1000; ++n) {
      for (u32 i = 0; i < 0x1000; i += 4) gw_store32(kSrc + i, (u32)rand() << 16 ^ (u32)rand());
      const u32 reg = (u32)(rand() % 128), count = (u32)(rand() % 40), src = kSrc + (u32)(rand() % 64);
      u64 dirty = 0;
      for (int b = 0; b < 4; ++b) dirty = dirty << 16 ^ (u64)(rand() & 0xFFFF);
      static u8 want[0x10000];
      for (int pass = 0; pass < 4; ++pass) {
        memset(gw_host(kDevice), 0x5A, 0x10000);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kDevice;
        ctx.r4.u64 = reg;
        ctx.r5.u64 = src;
        ctx.r6.u64 = count;
        ctx.r7.u64 = dirty;
        switch (pass) {
          case 0: original_sub_820A6438(ctx, gw_host(0)); break;
          case 1: sub_820A6438(ctx, gw_host(0)); break;
          case 2: original_sub_820A6518(ctx, gw_host(0)); break;
          case 3: sub_820A6518(ctx, gw_host(0)); break;
        }
        if (pass % 2 == 0) memcpy(want, gw_host(kDevice), 0x10000);
        else if (memcmp(want, gw_host(kDevice), 0x10000) != 0) ++differ;
      }
    }
    printf("shader constant shadows: %d of 2000 differ\n", differ);
    bad += differ;
  }

  // DXT5 texture compression: round trip of a text-like image (white glyphs,
  // alpha 0/255), a smooth gradient and noise. Text and gradient must come
  // back close; noise only shows the worst case.
  {
    struct Case {
      const char* name;
      int kind;
      float mean_limit;
    } cases[] = {{"text", 0, 4.0f}, {"gradient", 1, 3.0f}, {"noise", 2, 1000.0f}};
    for (const Case& c : cases) {
      double total = 0;
      int worst = 0;
      const int blocks = 4096;
      for (int n = 0; n < blocks; ++n) {
        u32 px[16], back[16];
        for (int i = 0; i < 16; ++i) {
          const int x = (n % 64) * 4 + (i & 3), y = (n / 64) * 4 + (i >> 2);
          if (c.kind == 0) {
            const bool ink = ((x * 7 + y * 13) % 11 < 4) ^ ((x / 3 + y / 5) & 1);
            px[i] = ink ? 0xFFFFFFFFu : 0x00FFFFFFu;
          } else if (c.kind == 1) {
            px[i] = 0xFF000000u | (u32)(x & 255) << 16 | (u32)(y & 255) << 8 | (u32)((x + y) / 2 & 255);
          } else {
            px[i] = (u32)rand() << 16 ^ (u32)rand();
          }
        }
        u8 block[16];
        gw_dxt5_block(px, block);
        gw_dxt5_decode_block(block, back);
        for (int i = 0; i < 16; ++i) {
          for (int s = 0; s < 32; s += 8) {
            const int e = abs((int)((px[i] >> s) & 255) - (int)((back[i] >> s) & 255));
            total += e;
            if (e > worst) worst = e;
          }
        }
      }
      const double mean = total / (blocks * 16 * 4);
      printf("DXT5 %-8s: mean error %.2f, worst %d (of 255)\n", c.name, mean, worst);
      if (mean > c.mean_limit) ++bad;
    }
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
