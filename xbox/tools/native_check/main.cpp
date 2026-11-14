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
extern "C" void sub_82061750(PPCContext& ctx, uint8_t* base);  // stubbed below
extern "C" void sub_82030338(PPCContext& ctx, uint8_t* base);  // native (math_native.cpp)
extern "C" void sub_82063448(PPCContext& ctx, uint8_t* base);  // native (particle_native.cpp)
extern "C" void sub_8209AC70(PPCContext& ctx, uint8_t* base);  // recorded below
extern "C" void sub_8209D288(PPCContext& ctx, uint8_t* base);  // native (d3d_native.cpp)
extern "C" void sub_820AA9E0(PPCContext& ctx, uint8_t* base);  // stand-ins below
extern "C" void sub_8212EDC0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205EB58(PPCContext& ctx, uint8_t* base);  // native (shapes_native.cpp)
extern "C" void sub_8212EA80(PPCContext& ctx, uint8_t* base);  // native (math_native.cpp)
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);  // native (math_native.cpp)
extern "C" void sub_82064060(PPCContext& ctx, uint8_t* base);  // the original, below
// Separation's random push: a stand-in, the same vectors in the same order
// for both versions (r3 the output vector, returned in r3).
int g_random_calls;
extern "C" void sub_8203B8B0(PPCContext& ctx, uint8_t*) {
  const float a = (float)(g_random_calls++ % 360) * 0.0174533f, len = (float)ctx.f1.f64;
  float v[2] = {cosf(a) * len, sinf(a) * len};
  for (int k = 0; k < 2; ++k) {
    u32 w;
    memcpy(&w, &v[k], 4);
    gw_store32(ctx.r3.u32 + k * 4, w);
  }
}
// src/enemy_native.cpp: the originals' calls to functions rewritten there go
// to the originals here (and the callees not rewritten to their extracted
// originals), so each check runs the recompiled code all the way down.
extern "C" void original_sub_8205A2B0(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_82059FC0(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_8205A100(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_8205A0F0(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_8205C5D8(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_8205C6D8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205A3E0(PPCContext& ctx, uint8_t* base);  // recorded below
extern "C" void sub_82046248(PPCContext& ctx, uint8_t* base);  // native (entity_native.cpp)
extern "C" void sub_82041600(PPCContext& ctx, uint8_t* base);  // native (math_native.cpp)
extern "C" void sub_82041CA0(PPCContext& ctx, uint8_t* base);  // native (math_native.cpp)
extern "C" void sub_82040F28(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_82052860(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_8212FE70(PPCContext& ctx, uint8_t* base);  // atan2: native (enemy_native.cpp)
extern "C" void sub_82030220(PPCContext& ctx, uint8_t* base);  // random float: stand-in below
extern "C" void sub_82030058(PPCContext& ctx, uint8_t* base);  // random integer: stand-in below
extern "C" void sub_820617E8(PPCContext& ctx, uint8_t* base);  // sprite vertices: stand-in below
extern "C" void sub_820787F8(PPCContext& ctx, uint8_t* base);  // play a sound: stand-in below
extern "C" void sub_82030430(PPCContext& ctx, uint8_t* base);  // random float: stand-in below
extern "C" void sub_8212FC00(PPCContext& ctx, uint8_t* base);  // CRT routine: stand-in below
extern "C" void sub_8205FA98(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void original_sub_82045AB8(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_8204F458(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_8205FA98(PPCContext& ctx, uint8_t* base);
#define sub_8205FA98 original_sub_8205FA98
extern "C" void original_sub_82074538(PPCContext& ctx, uint8_t* base);
#define sub_82074538 original_sub_82074538
extern "C" void original_sub_82045628(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_82061908(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_82061A30(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205EF18(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_8205EB58(PPCContext& ctx, uint8_t* base);  // native (shapes_native.cpp)
#define sub_82045628 original_sub_82045628
#define sub_82061908 original_sub_82061908
#define sub_82061A30 original_sub_82061A30
extern "C" void sub_8209D388(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_82067A20(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_82047AA8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_82030AC0(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_820614F8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_8204F4F8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_8204FBA8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_820749F8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_82045740(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void original_sub_8206A570(PPCContext& ctx, uint8_t* base);
#define sub_8206A570 original_sub_8206A570
extern "C" void sub_82035A40(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_8204D2E8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_82045CF0(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_82061DA0(PPCContext& ctx, uint8_t* base);  // native (shapes_native.cpp)
extern "C" void original_sub_82040FF0(PPCContext& ctx, uint8_t* base);
#define sub_82040FF0 original_sub_82040FF0
extern "C" void sub_82047180(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_82062FC8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_82030168(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_82067388(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_82030678(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_8206A1C8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_8206A300(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_8209C1B8(PPCContext& ctx, uint8_t* base);  // logged stand-in below
extern "C" void sub_820A6438(PPCContext& ctx, uint8_t* base);  // native (d3d_native.cpp)
extern "C" void sub_820304C8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82030A98(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82033610(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82033638(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82034408(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82034588(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82034CA0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820359E0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82036358(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820364D0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8203BC08(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8203E7E8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82043368(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820476F0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82048638(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820499E0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204A9C0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204AD38(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204B770(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204D410(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204EBE8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204F3B0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204F3F8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8204FAD8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82051448(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820523B0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8205C8F8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8205CA98(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8205E068(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8205E1E0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82067390(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82078970(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82090DA8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820E8130(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82033600(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8205D740(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82076130(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820762F0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820763D0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820764C0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820765A8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82076778(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82076E40(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820770E8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8207C3A0(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8207C530(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8207C790(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8207E238(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8207E248(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820826D8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8208BBF8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_8208BCF8(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_82093600(PPCContext& ctx, uint8_t* base);  // answering stand-in below
extern "C" void sub_820453E0(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
extern "C" void sub_82030C58(PPCContext& ctx, uint8_t* base);  // native (enemy_native.cpp)
#define sub_82045AB8 original_sub_82045AB8
#define sub_8204F458 original_sub_8204F458
#define sub_8205A2B0 original_sub_8205A2B0
#define sub_82059FC0 original_sub_82059FC0
#define sub_8205A100 original_sub_8205A100
#define sub_8205A0F0 original_sub_8205A0F0
#define sub_8205C5D8 original_sub_8205C5D8
#define sub_8205C6D8 original_sub_8205C6D8
#include "original.inc"
#undef sub_8205FA98
#undef sub_82074538
#undef sub_82045628
#undef sub_82061908
#undef sub_82061A30
#undef sub_8206A570
#undef sub_82040FF0
#undef sub_82045AB8
#undef sub_8204F458
#undef sub_8205A2B0
#undef sub_82059FC0
#undef sub_8205A100
#undef sub_8205A0F0
#undef sub_8205C5D8
#undef sub_8205C6D8

extern "C" void sub_8205B880(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8212E9B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8212EA80(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82041600(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82063448(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82075548(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82063A38(PPCContext& ctx, uint8_t* base);
void gw_particle_store_swap(u32 buckets, u32 count);  // src/particle_native.cpp
extern "C" void sub_82063100(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82063018(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8209D288(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A68B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A65F8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82061DA0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8209D8A0(PPCContext& ctx, uint8_t* base);
// The shader sets' callees: making room hands back a fixed buffer; the copy
// is a guest memcpy (r3 destination, r4 source, r5 bytes).
constexpr u32 kRoomBuffer = 0x70430000;
extern "C" void sub_820AA9E0(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kRoomBuffer; }
extern "C" void sub_8212EDC0(PPCContext& ctx, uint8_t*) {
  memmove(gw_host(ctx.r3.u32), gw_host(ctx.r4.u32), ctx.r5.u32);
}
// The scissor's callee: records the corners it was given.
u64 g_scissor_args[5];
extern "C" void sub_8209AC70(PPCContext& ctx, uint8_t*) {
  g_scissor_args[0] = ctx.r4.u64;
  g_scissor_args[1] = ctx.r5.u64;
  g_scissor_args[2] = ctx.r6.u64;
  g_scissor_args[3] = ctx.r7.u64;
  g_scissor_args[4] = ctx.r1.u64;
}
extern "C" void sub_82064110(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82064060(PPCContext& ctx, uint8_t* base) { original_sub_82064060(ctx, base); }
extern "C" void __imp__sub_82064110(PPCContext& ctx, uint8_t* base) { original_sub_82064110(ctx, base); }
extern "C" void sub_820A91A0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_821308B8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82096F30(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A6438(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820A6518(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205FBA8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205EB58(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030338(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82041CA0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82046248(PPCContext& ctx, uint8_t* base);
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
extern "C" void __imp__sub_8212EA80(PPCContext& ctx, uint8_t* base) { original_sub_8212EA80(ctx, base); }

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
u8* gw_grid_line_block(u32, u32) { return nullptr; }
// Host mirrors for the particle tests (the native update writes the glow
// splat only into a mirror, and the streak into one when there is one):
// mirror 0 at g_test_mirror_guest, mirror 1 at g_test_mirror2_guest.
u32 g_test_mirror_guest, g_test_mirror_size, g_test_mirror2_guest, g_test_mirror2_size;
alignas(16) u8 g_test_mirror_host[8192];
alignas(16) u8 g_test_mirror2_host[8192];
u8* gw_vertex_mirror(u32 guest, u32 bytes) {
  const u32 at = guest - g_test_mirror_guest, at2 = guest - g_test_mirror2_guest;
  if (g_test_mirror_size && at < g_test_mirror_size && bytes <= g_test_mirror_size - at) return g_test_mirror_host + at;
  if (g_test_mirror2_size && at2 < g_test_mirror2_size && bytes <= g_test_mirror2_size - at2)
    return g_test_mirror2_host + at2;
  return nullptr;
}
u8* gw_vertex_mirror_add(u32, u32) { return nullptr; }
u32 gw_vertex_mirror_room(u32 guest) {
  const u32 at = guest - g_test_mirror_guest, at2 = guest - g_test_mirror2_guest;
  if (g_test_mirror_size && at < g_test_mirror_size) return g_test_mirror_size - at;
  if (g_test_mirror2_size && at2 < g_test_mirror2_size) return g_test_mirror2_size - at2;
  return 0;
}
bool gw_frame_drawn() { return true; }  // every frame drawn here
u32 gw_drawn_index() { return 0; }
u32 gw_particle_set_buffer(u32, u32, bool) { return 0; }  // the game's tables as they are
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
constexpr u32 kPart = 0x70400000;        // a particle system, buckets, index (the shares' test)
constexpr u32 kVerts = 0x71000000;       // the shares' vertex buffers (12 x 256 KB)
constexpr u32 kLineQuad = 0xC0780000, kLineOutline = 0xC0781000;  // the line builder's two streams
float random_float(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }
// The line builder's vertex allocator (guest 0x82061750), stubbed: fixed
// buffers for its two streams.
u32 g_alloc_stream, g_alloc_buffer;  // when set: that stream's vertices go to that buffer
extern "C" void sub_82061750(PPCContext& ctx, uint8_t*) {
  const u32 stream = ctx.r3.u32;
  if (g_alloc_stream && stream == g_alloc_stream) {
    ctx.r3.u64 = g_alloc_buffer;
    return;
  }
  ctx.r3.u64 = stream == 0x821A0000 + 16648 || stream == 0x821A0000 + 12424 ? kLineQuad : kLineOutline;
}

float frand(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }
// The same value for the same key, in both passes of a check.
float frand_fixed(u32 key, float lo, float hi) {
  u32 h = key * 2654435761u;
  h ^= h >> 15;
  return lo + (hi - lo) * (float)(h & 0xFFFF) / 65535.0f;
}
float ldf_host(u32 a) {
  const u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
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

// --- src/enemy_native.cpp's checks: stand-ins and a call log -------------
extern "C" void sub_82052860(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82046BD8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820631E8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205A2B0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205C5D8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205C6D8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820453E0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82047180(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204F458(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82040F28(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82056CD8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82059838(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82049E60(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82056B48(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82045AB8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82059CD0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82060CC0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82045B18(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82044330(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82054E30(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82044168(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82074B28(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82045740(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030678(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82065E38(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205F6B8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204D8E0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82056FA0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82046D88(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82047DF8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82045CF0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030C58(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82067A20(PPCContext& ctx, uint8_t* base);
extern "C" void original_sub_8205EF18(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820366A0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8207CA48(PPCContext& ctx, uint8_t* base);
// Every stand-in call appends what it was given here; both versions must
// leave the same log.
u32 g_enemy_log[1 << 16];
u32 g_enemy_log_n;
void enemy_log(u32 v) {
  if (g_enemy_log_n < (1u << 16)) g_enemy_log[g_enemy_log_n++] = v;
}
// The pushers' counting sort (guest), called last by sub_820631E8: logged.
extern "C" void sub_8205A3E0(PPCContext& ctx, uint8_t*) {
  enemy_log(0xA3E0);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r1.u32);
}
// Entity::Tick's virtual calls (registered in the dispatch table at fake
// addresses): each logs its arguments and the stack pointer, and changes the
// entity a little (so later steps see the change).
constexpr u32 kFakeUpdate = 0x7FFF0010, kFakeAfter = 0x7FFF0020, kFakePushed = 0x7FFF0030, kFakeTouch = 0x7FFF0040;
void fake_update(PPCContext& ctx, uint8_t*) {
  const u32 e = ctx.r3.u32;
  enemy_log(1);
  enemy_log(e);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  sf(e + 12, lf(e + 12) * 0.75f + 1.5f);
  // Sometimes toggles the flags Tick reads after the update.
  const u32 f = gw_load32(e + 36);
  if ((f >> 9) & 1) gw_store32(e + 36, f ^ 0x80A);
}
void fake_after(PPCContext& ctx, uint8_t*) {
  enemy_log(2);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r1.u32);
  sf(ctx.r3.u32 + 8, lf(ctx.r3.u32 + 8) - 3.0f);
}
void fake_pushed(PPCContext& ctx, uint8_t*) {
  enemy_log(3);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log(ctx.r1.u32);
  enemy_log(gw_load32(ctx.r4.u32));
  enemy_log(gw_load32(ctx.r4.u32 + 4));
  sf(ctx.r3.u32 + 12, lf(ctx.r3.u32 + 12) + lf(ctx.r4.u32) * 4);
}
// atan2 (guest CRT): the native one (src/enemy_native.cpp), checked by check_atan2.
// The game's random float (r3 the generator, f1 the range) and integer.
extern "C" void sub_82030220(PPCContext& ctx, uint8_t*) {
  enemy_log(0x0220);
  enemy_log(ctx.r3.u32);
  enemy_log((u32)(ctx.f1.u64 >> 32));
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  ctx.f1.f64 = ctx.f1.f64 * (double)(g_random_calls++ % 101) / 101.0;
}
void clobber_volatiles(PPCContext& ctx, u32 h);
u32 volatile_hash(const PPCContext& ctx);
bool g_log_volatiles;
// (stand-ins below leave junk in the volatile registers, as a callee may)
extern "C" void sub_82030058(PPCContext& ctx, uint8_t*) {
  enemy_log(0x0058);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  ctx.r3.u64 = 0xDEAD0000ull << 32 | (u32)(0x9E3779B9u * (u32)++g_random_calls);
  if (g_log_volatiles) enemy_log(volatile_hash(ctx));  // (the world update's and score display's cases)
  clobber_volatiles(ctx, 0x5EED0000u + g_enemy_log_n);
}
// The sprite stream's allocator: a fixed buffer, its arguments logged.
constexpr u32 kSpriteVerts = 0xC0784000;
extern "C" void sub_820617E8(PPCContext& ctx, uint8_t*) {
  enemy_log(0x17E8);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  ctx.r3.u64 = kSpriteVerts;
}
// The CRT routine the glow calls with (3 x timer, 1.0): pow here, logged.
extern "C" void sub_8212FC00(PPCContext& ctx, uint8_t*) {
  enemy_log(0xFC00);
  enemy_log((u32)(ctx.f1.u64 >> 32));
  enemy_log((u32)ctx.f1.u64);
  enemy_log((u32)(ctx.f2.u64 >> 32));
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  ctx.f1.f64 = pow(ctx.f1.f64, ctx.f2.f64) * 0.75;
}
// Play a sound (r3 its number): logged.
extern "C" void sub_820787F8(PPCContext& ctx, uint8_t*) {
  enemy_log(0x87F8);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  if (g_log_volatiles) enemy_log(volatile_hash(ctx));  // (the world update's and score display's cases)
  clobber_volatiles(ctx, 0x5EED0000u + g_enemy_log_n);
}
// The game's other random float (r3 the generator, f1 the range): logged.
extern "C" void sub_82030430(PPCContext& ctx, uint8_t*) {
  enemy_log(0x0430);
  enemy_log(ctx.r3.u32);
  enemy_log((u32)(ctx.f1.u64 >> 32));
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  ctx.f1.f64 = ctx.f1.f64 * (double)(g_random_calls++ % 89) / 89.0;
}
// (The second line style, 0x8205EF18, is native: enemy_native.cpp, check_line_style2.)
// The draw dispatch's virtuals +4 and +8: logged.
constexpr u32 kFakeDraw4 = 0x7FFF0060, kFakeDraw8 = 0x7FFF0070;
void fake_draw4(PPCContext& ctx, uint8_t*) {
  enemy_log(6);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
}
void fake_draw8(PPCContext& ctx, uint8_t*) {
  enemy_log(7);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
}
// The scene draw's callees other than the entity drawing: logged stand-ins.
#define GW_LOGGED_STUB(addr)                                   \
  extern "C" void sub_##addr(PPCContext& ctx, uint8_t*) {      \
    enemy_log(0x##addr);                                       \
    enemy_log(ctx.r3.u32);                                     \
    enemy_log(ctx.r4.u32);                                     \
    enemy_log(ctx.r8.u32);                                     \
    enemy_log((u32)(ctx.f1.u64 >> 32));                        \
    enemy_log(ctx.r1.u32);                                     \
    enemy_log((u32)ctx.lr);                                    \
  }
GW_LOGGED_STUB(8209D388)
GW_LOGGED_STUB(82047AA8)
GW_LOGGED_STUB(82030AC0)
GW_LOGGED_STUB(820614F8)
GW_LOGGED_STUB(8204F4F8)
GW_LOGGED_STUB(8204FBA8)
GW_LOGGED_STUB(820749F8)
GW_LOGGED_STUB(82035A40)
GW_LOGGED_STUB(8204D2E8)
#undef GW_LOGGED_STUB
// An entity's own matrix virtual (+24): logged, fills a matrix.
constexpr u32 kFakeMatrix = 0x7FFF0080;
void fake_matrix(PPCContext& ctx, uint8_t*) {
  enemy_log(8);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  for (u32 k = 0; k < 16; ++k) sf(ctx.r3.u32 + k * 4, (float)k * 0.25f);
}
// The explosion burst's callees: the particle store's check (answers
// g_store_full), a random vector (r3 out, f1 its length), add a particle
// (r3 the set, r4 its direction, r5 its colour); all logged.
u32 g_store_full;
extern "C" void sub_82062FC8(PPCContext& ctx, uint8_t*) {
  enemy_log(0x2FC8);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  ctx.r3.u64 = g_store_full;
}
extern "C" void sub_82030168(PPCContext& ctx, uint8_t*) {
  enemy_log(0x0168);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log((u32)(ctx.f1.u64 >> 32));
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  const float a = (float)(g_random_calls++ % 97) * 0.07f, len = (float)ctx.f1.f64;
  sf(ctx.r3.u32 + 0, cosf(a) * len);
  sf(ctx.r3.u32 + 4, sinf(a) * len);
  sf(ctx.r3.u32 + 8, sinf(a * 3) * len);
  sf(ctx.r3.u32 + 12, 0.0f);
  if (g_log_volatiles) enemy_log(volatile_hash(ctx));  // (the world update's and score display's cases)
  clobber_volatiles(ctx, 0x5EED0000u + g_enemy_log_n);
}
extern "C" void sub_82067388(PPCContext& ctx, uint8_t*) {
  enemy_log(0x7388);
  enemy_log(ctx.r3.u32);
  for (u32 k = 0; k < 16; k += 4) enemy_log(gw_load32(ctx.r4.u32 + k));
  for (u32 k = 0; k < 16; k += 4) enemy_log(gw_load32(ctx.r5.u32 + k));
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  if (g_log_volatiles) enemy_log(volatile_hash(ctx));  // (the world update's and score display's cases)
  clobber_volatiles(ctx, 0x5EED0000u + g_enemy_log_n);
}
// The camera's callees: the projection helpers (r3 the matrix out; r4 the
// view / f1..f4 its parameters) fill it from what they get; all logged.
extern "C" void sub_8206A1C8(PPCContext& ctx, uint8_t*) {
  enemy_log(0xA1C8);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  for (u32 k = 0; k < 16; ++k) sf(ctx.r3.u32 + k * 4, lf(ctx.r4.u32 + ((k * 5) % 16) * 4) * 0.5f + (float)k);
}
extern "C" void sub_8206A300(PPCContext& ctx, uint8_t*) {
  enemy_log(0xA300);
  enemy_log(ctx.r3.u32);
  const double args[4] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64};
  for (const double v : args) {
    u64 b;
    memcpy(&b, &v, 8);
    enemy_log((u32)(b >> 32));
    enemy_log((u32)b);
  }
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  for (u32 k = 0; k < 16; ++k) sf(ctx.r3.u32 + k * 4, (float)(ctx.f1.f64 * (k + 1) - ctx.f2.f64 * k + ctx.f4.f64 * 1e-4));
}
extern "C" void sub_8209C1B8(PPCContext& ctx, uint8_t*) {
  enemy_log(0xC1B8);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
}
// The world update's and the score display's callees: stand-ins that log
// what they get and answer from a fixed sequence (r3 a small number, f1 a
// value), the same in both versions' runs.
u32 g_answer_n;
// All the volatile registers (r0, r3-r12, f0-f13), all 64 bits, hashed.
// What a callee may leave in the volatile registers it does not return in:
// deterministic junk from h (r0, r4-r12, f0, f2-f13).
void clobber_volatiles(PPCContext& ctx, u32 h) {
  PPCRegister* r[] = {&ctx.r0, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10, &ctx.r11, &ctx.r12};
  for (PPCRegister* p : r) p->u64 = (h = h * 1103515245u + 12345u) | (u64)0xC1A0 << 48;
  PPCRegister* f[] = {&ctx.f0, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8, &ctx.f9, &ctx.f10,
                      &ctx.f11, &ctx.f12, &ctx.f13};
  for (PPCRegister* p : f) p->f64 = (double)((h = h * 1103515245u + 12345u) % 1000) * 0.37;
}
u32 volatile_hash(const PPCContext& ctx) {
  const PPCRegister* r[] = {&ctx.r0, &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10, &ctx.r11, &ctx.r12,
                            &ctx.f0, &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8, &ctx.f9, &ctx.f10,
                            &ctx.f11, &ctx.f12, &ctx.f13};
  u32 h = 2166136261u;
  for (const PPCRegister* p : r) {
    u64 v = p->u64;
    for (int k = 0; k < 8; ++k, v >>= 8) h = (h ^ (u32)(v & 0xFF)) * 16777619u;
  }
  return h;
}
constexpr u32 kAnswerArea = 0x70600000 + 0x7C000;  // (T + 0x7C000: zeroed for each run, below the test stack)
#define GW_ANSWERING_STUB(addr)                                     \
  extern "C" void sub_##addr(PPCContext& ctx, uint8_t*) {           \
    enemy_log(0x##addr);                                            \
    enemy_log(volatile_hash(ctx));                                  \
    enemy_log(ctx.r1.u32);                                          \
    enemy_log((u32)ctx.lr);                                         \
    const u32 h = (++g_answer_n * 2654435761u) >> 16;                \
    ctx.r3.u64 = kAnswerArea + (h % 8) * 128; /* low byte 0 half the time */\
    ctx.f1.f64 = (double)(h % 1000) * 0.001;                        \
    clobber_volatiles(ctx, h);                                      \
  }
GW_ANSWERING_STUB(820304C8)
GW_ANSWERING_STUB(82030A98)
GW_ANSWERING_STUB(82033610)
GW_ANSWERING_STUB(82033638)
GW_ANSWERING_STUB(82034408)
GW_ANSWERING_STUB(82034588)
GW_ANSWERING_STUB(82034CA0)
GW_ANSWERING_STUB(820359E0)
GW_ANSWERING_STUB(82036358)
GW_ANSWERING_STUB(820364D0)
GW_ANSWERING_STUB(8203BC08)
GW_ANSWERING_STUB(8203E7E8)
GW_ANSWERING_STUB(82043368)
GW_ANSWERING_STUB(820476F0)
GW_ANSWERING_STUB(82048638)
GW_ANSWERING_STUB(820499E0)
GW_ANSWERING_STUB(8204A9C0)
GW_ANSWERING_STUB(8204AD38)
GW_ANSWERING_STUB(8204B770)
GW_ANSWERING_STUB(8204D410)
GW_ANSWERING_STUB(8204EBE8)
GW_ANSWERING_STUB(8204F3B0)
GW_ANSWERING_STUB(8204F3F8)
GW_ANSWERING_STUB(8204FAD8)
GW_ANSWERING_STUB(82051448)
GW_ANSWERING_STUB(820523B0)
GW_ANSWERING_STUB(8205C8F8)
GW_ANSWERING_STUB(8205CA98)
GW_ANSWERING_STUB(8205E068)
GW_ANSWERING_STUB(8205E1E0)
GW_ANSWERING_STUB(82067390)
GW_ANSWERING_STUB(82078970)
GW_ANSWERING_STUB(82090DA8)
GW_ANSWERING_STUB(820E8130)
GW_ANSWERING_STUB(82033600)
GW_ANSWERING_STUB(8205D740)
GW_ANSWERING_STUB(82076130)
GW_ANSWERING_STUB(820762F0)
GW_ANSWERING_STUB(820763D0)
GW_ANSWERING_STUB(820764C0)
GW_ANSWERING_STUB(820765A8)
GW_ANSWERING_STUB(82076778)
GW_ANSWERING_STUB(82076E40)
GW_ANSWERING_STUB(820770E8)
GW_ANSWERING_STUB(8207C3A0)
GW_ANSWERING_STUB(8207C530)
GW_ANSWERING_STUB(8207C790)
GW_ANSWERING_STUB(8207E238)
GW_ANSWERING_STUB(8207E248)
GW_ANSWERING_STUB(820826D8)
GW_ANSWERING_STUB(8208BBF8)
GW_ANSWERING_STUB(8208BCF8)
GW_ANSWERING_STUB(82093600)
#undef GW_ANSWERING_STUB
constexpr u32 kFakeExpired = 0x7FFF0050;
void fake_expired(PPCContext& ctx, uint8_t*) {
  enemy_log(5);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log(ctx.r1.u32);
  enemy_log((u32)ctx.lr);
  for (u32 k = 0; k < 12; k += 4) enemy_log(gw_load32(ctx.r4.u32 + k));
  sf(ctx.r3.u32 + 16, 2.5f);
}
void fake_touch(PPCContext& ctx, uint8_t*) {
  enemy_log(4);
  enemy_log(ctx.r3.u32);
  enemy_log(ctx.r4.u32);
  enemy_log(ctx.r1.u32);
  sf(ctx.r4.u32 + 4, lf(ctx.r4.u32 + 4) + 1.0f);  // moves the toucher: later checks see it
}

LONG WINAPI report_fault(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD* r = e->ExceptionRecord;
  u32 host = (u32)r->ExceptionInformation[1];
  printf("fault at host code %p touching host %08X (guest %08X)\n", r->ExceptionAddress, host, host + 0x5E000000u);
  exit(2);
}

// (A function of its own: main's stack frame grew past what start-up allows.)
__declspec(noinline) int check_particle_store() {
  // The particle store's add (0x82063100) and bucket clear (0x82063018):
  // random systems, original and native, compared with the native store
  // turned back to the guest's order.
  {
    constexpr u32 kSys = kPart, kBuckets = kPart + 0x1000, kIndex = kPart + 0x30000, kArgsAt = kPart + 0x31000;
    int differ_add = 0;
    for (int round = 0; round < 400; ++round) {
      const u32 n = 2 + rand() % 8;
      gw_store32(kSys + 0, n);
      gw_store32(kSys + 4, kBuckets);
      gw_store32(kSys + 8, kIndex);
      gw_store32(kSys + 12, (u32)(rand() % n));
      // Entries up to the current one: buckets in use (their numbers); after
      // it: free buckets, biased by n (as the job leaves them).
      const u32 current = gw_load32(kSys + 12);
      // The index: the buckets in a shuffled order; a free bucket (one the add
      // moves on to) is empty, as the job leaves them.
      u32 order[16];
      for (u32 b = 0; b < n; ++b) order[b] = b;
      for (u32 b = n - 1; b > 0; --b) {
        const u32 o = (u32)rand() % (b + 1), t = order[b];
        order[b] = order[o];
        order[o] = t;
      }
      for (u32 at = 0; at < n; ++at) {
        gw_store32(kIndex + at * 4, (at <= current ? 0 : n) + order[at]);
        const u32 bucket = kBuckets + order[at] * 5636;
        for (u32 w = 0; w < 128 * 11; ++w) sf(bucket + w * 4, frand(-500, 500));
        gw_store32(bucket + 5632, at > current ? 0 : rand() % 3 == 0 ? 128 : (u32)(rand() % 129));
      }
      for (u32 w = 0; w < 9; ++w) sf(kArgsAt + w * 4, frand(-3000, 3000));
      const u32 target = kBuckets + (u32)(rand() % n) * 5636;
      u8* before = (u8*)malloc(0x32000);
      u8* want = (u8*)malloc(0x32000);
      memcpy(before, gw_host(kPart), 0x32000);
      for (int pass = 0; pass < 2; ++pass) {
        memcpy(gw_host(kPart), before, 0x32000);
        if (pass == 1) gw_particle_store_swap(kBuckets, n);
        for (int call = 0; call < 3; ++call) {
          static PPCContext ctx;
          ctx = PPCContext{};
          ctx.r1.u64 = kStack;
          ctx.r3.u64 = kSys;
          ctx.r4.u64 = kArgsAt;
          ctx.r5.u64 = kArgsAt + 12;
          ctx.r6.u64 = kArgsAt + 24;
          if (pass == 0) original_sub_82063100(ctx, gw_host(0));
          else sub_82063100(ctx, gw_host(0));
        }
        if (round % 4 == 0) {
          static PPCContext ctx;
          ctx = PPCContext{};
          ctx.r1.u64 = kStack;
          ctx.r3.u64 = target;
          if (pass == 0) original_sub_82063018(ctx, gw_host(0));
          else sub_82063018(ctx, gw_host(0));
        }
        if (pass == 1) gw_particle_store_swap(kBuckets, n);
        if (pass == 0) memcpy(want, gw_host(kPart), 0x32000);
        else differ_add += memcmp(want, gw_host(kPart), 0x32000) != 0;
      }
      free(before);
      free(want);
    }
    printf("particle add and bucket clear (0x82063100, 0x82063018): %d of 400 differ\n", differ_add);
    return differ_add;
  }
  return 0;
}

// --- src/enemy_native.cpp's checks ----------------------------------------
// Each function on random inputs in a test area, run by the original, then
// (inputs restored) by the native version; the test area, the stand-ins'
// call log and the result registers compared byte for byte. The image's
// data words the functions read are set by enemy_setup() and the whole
// image restored by enemy_teardown().
namespace {

constexpr u32 T = 0x70600000, kTSize = 0x80000;
constexpr u32 kHashT = T, kEntriesT = T + 0x4000, kEntsT = T + 0x10000, kHandlesT = T + 0x30000;
constexpr u32 kVtbl = T + 0x31000, kNodesT = T + 0x32000, kPushEntries = T + 0x40000, kPushObj = T + 0x58000;
// kMisc: the small cases' inputs; the stack (below kSp) is the originals'
// scratch and never compared.
constexpr u32 kMisc = T + 0x3F000, kSp = T + 0x7F000, kArea = 0x40000;
constexpr u32 kEntSz = 256;
u8* g_enemy_image;
u8 tin[kTSize], twant[kTSize];
u32 want_log[1 << 16];
// Time in each version's calls (this PC; relative only), per case.
double pass_time[2];
LARGE_INTEGER qpf;

bool enemy_setup() {
  if (!map(T, kTSize)) return false;
  g_enemy_image = (u8*)malloc(0x220000);
  memcpy(g_enemy_image, gw_host(0x82000000), 0x220000);
  QueryPerformanceFrequency(&qpf);
  // The screen and grid layout (as setup() uses) and the frame's step.
  sf(K + 1540, 640);
  sf(K + 1544, 360);
  gw_store32(K + 1556, 16);
  gw_store32(K + 1560, 16);
  sf(D + 9640, 16.0f / 1280);
  sf(D + 9644, 16.0f / 720);
  sf(D + 9652, 640);
  sf(D + 9656, 360);
  sf(0x8219D280, 1.0f / 60);
  gw_dispatch_add(kFakeUpdate, fake_update);
  gw_dispatch_add(kFakeAfter, fake_after);
  gw_dispatch_add(kFakePushed, fake_pushed);
  gw_dispatch_add(kFakeTouch, fake_touch);
  gw_store32(kVtbl + 12, kFakeUpdate);
  gw_store32(kVtbl + 16, kFakeAfter);
  gw_store32(kVtbl + 20, kFakePushed);
  gw_store32(kVtbl + 48, kFakeTouch);
  gw_dispatch_add(kFakeExpired, fake_expired);
  gw_store32(kVtbl + 36, kFakeExpired);
  gw_dispatch_add(kFakeDraw4, fake_draw4);
  gw_dispatch_add(kFakeDraw8, fake_draw8);
  gw_store32(kVtbl + 4, kFakeDraw4);
  gw_store32(kVtbl + 8, kFakeDraw8);
  return true;
}

void enemy_teardown() {
  memcpy(gw_host(0x82000000), g_enemy_image, 0x220000);
  free(g_enemy_image);
}

struct Regs {
  u64 v[4];
};

const char* speed() {
  static char text[64];
  snprintf(text, sizeof(text), " [%.1fx on this PC]", pass_time[1] > 0 ? pass_time[0] / pass_time[1] : 0.0);
  pass_time[0] = pass_time[1] = 0;
  return text;
}

// Runs call(pass, ctx) twice on the same test area; returns whether the
// area, the log and the registers regs(ctx) picked agree.
template <typename Call, typename Pick>
bool twice(u32 at, u32 size, Call&& call, Pick&& regs) {
  memcpy(tin, gw_host(at), size);
  Regs want{};
  u32 want_n = 0;
  for (int pass = 0; pass < 2; ++pass) {
    memcpy(gw_host(at), tin, size);
    g_enemy_log_n = 0;
    static PPCContext ctx;
    ctx = PPCContext{};
    ctx.r1.u64 = kSp;
    ctx.lr = 0x12345678;
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    call(pass, ctx);
    QueryPerformanceCounter(&q1);
    pass_time[pass] += (double)(q1.QuadPart - q0.QuadPart) / qpf.QuadPart;
    Regs got = regs(ctx);
    got.v[3] ^= ctx.lr << 32;  // the link register too, as the original leaves it
    if (pass == 0) {
      want = got;
      memcpy(twant, gw_host(at), size);
      want_n = g_enemy_log_n;
      memcpy(want_log, g_enemy_log, want_n * 4);
    } else {
      if (memcmp(twant, gw_host(at), size) != 0) {
        static int shown;
        for (u32 o = 0; o < size && shown < 6; o += 4)
          if (memcmp(twant + o, gw_host(at + o), 4) != 0) {
            printf("    first differences: guest %08X original %08X native %08X\n", at + o,
                   __builtin_bswap32(*(u32*)(twant + o)), gw_load32(at + o));
            ++shown;
          }
        return false;
      }
      if (want_n != g_enemy_log_n || memcmp(want_log, g_enemy_log, want_n * 4) != 0) {
        static int shown;
        if (shown++ < 3) {
          u32 i = 0;
          while (i < want_n && i < g_enemy_log_n && want_log[i] == g_enemy_log[i]) ++i;
          printf("    call logs differ at entry %u of %u/%u: original %08X native %08X\n", i, want_n,
                 g_enemy_log_n, i < want_n ? want_log[i] : 0, i < g_enemy_log_n ? g_enemy_log[i] : 0);
        }
        return false;
      }
      if (memcmp(&want, &got, sizeof(want)) != 0) return false;
    }
  }
  return true;
}

u64 f64bits(double d) {
  u64 v;
  memcpy(&v, &d, 8);
  return v;
}

// An entity list {entity, next} ending in a node with entity 0, its head
// word at head + 4.
void make_list(u32 head, u32 nodes, const u32* ents, u32 n) {
  for (u32 i = 0; i < n; ++i) {
    gw_store32(nodes + i * 8, ents[i]);
    gw_store32(nodes + i * 8 + 4, nodes + (i + 1) * 8);
  }
  gw_store32(nodes + n * 8, 0);
  gw_store32(nodes + n * 8 + 4, 0);
  gw_store32(head + 4, nodes);
}

// A 32 x 32 hash in kHashT with entities in kEntsT inserted at the front of
// their cells.
void make_hash(u32 count) {
  for (u32 c = 0; c < 1024; ++c) {
    gw_store32(kHashT + c * 16 + 4, kHashT + c * 16);
    gw_store32(kHashT + c * 16 + 8, kHashT + c * 16);
  }
  for (u32 i = 0; i < count; ++i) {
    const u32 e = kEntsT + i * kEntSz;
    const float x = lf(e + 4), y = lf(e + 8);
    const u32 c = ((((u32)(i32)y >> 5) & 31) << 5 | (((u32)(i32)x >> 5) & 31));
    const u32 cellp = kHashT + c * 16, entry = kEntriesT + i * 8;
    gw_store32(entry, e);
    gw_store32(entry + 4, gw_load32(cellp + 4));
    gw_store32(cellp + 4, entry);
  }
}

void random_entities(u32 count, float spread) {
  memset(gw_host(kEntsT), 0, count * kEntSz);
  for (u32 i = 0; i < count; ++i) {
    const u32 e = kEntsT + i * kEntSz;
    float x = frand(-spread, spread), y = frand(-spread, spread);
    if (i % 9 == 4) {
      x = frand(-660, 660) * (rand() & 1 ? 1 : -1);
      y = frand(-380, 380);
    }
    sf(e + 4, x);
    sf(e + 8, y);
    const int still = rand() % 13 == 0;  // no velocity: the scores' 0/0
    sf(e + 12, still ? 0.0f : frand(-6, 6));
    sf(e + 16, still ? 0.0f : frand(-6, 6));
    sf(e + 32, frand(2, 40));
    gw_store32(e + 36, (u32)rand() << 16 ^ (u32)rand());
    sf(e + 56, frand(0, 1.2f));
    sf(e + 224, rand() % 4 == 0 ? frand(0.1f, 3) : 0.0f);
    gw_store32(e + 0, kVtbl);
    gw_store32(e + 68, rand() % 3);
  }
}

}  // namespace


  // Nearest target (0x82052860).
__declspec(noinline) int check_enemy_1() {
  int bad = 0;
    const u32 saved = gw_load32(0x8219F9AC);
    gw_store32(0x8219F9AC, kHashT);
    int differ = 0;
    for (int n = 0; n < 60; ++n) {
      random_entities(400, 560);
      for (u32 i = 0; i < 400; ++i) {
        u32 f = gw_load32(kEntsT + i * kEntSz + 36);
        if (rand() % 4) f &= ~0x200001u;
        gw_store32(kEntsT + i * kEntSz + 36, f);
      }
      make_hash(400);
      for (int k = 0; k < 40; ++k) {
        sf(kMisc, frand(-600, 600));
        sf(kMisc + 4, frand(-600, 600));
        const double r = k % 10 == 9 ? (double)frand(1, 400) + 1e-9 : (double)frand(1, 400);
        const bool ok = twice(kMisc, 0x1000,
            [&](int pass, PPCContext& ctx) {
              ctx.r3.u64 = kMisc;
              ctx.f1.f64 = r;
              if (pass == 0) original_sub_82052860(ctx, gw_host(0));
              else sub_82052860(ctx, gw_host(0));
            },
            [&](PPCContext& ctx) { return Regs{{ctx.r3.u64, f64bits(ctx.f1.f64), f64bits(ctx.f2.f64), 0}}; });
        if (!ok) ++differ;
      }
    }
    gw_store32(0x8219F9AC, saved);
    printf("nearest target (0x82052860): %d of 2400 differ%s\n", differ, speed());
    if (differ) ++bad;
  return bad;
}

  // Hash cell and handle flags (0x82046BD8).
__declspec(noinline) int check_enemy_2() {
  int bad = 0;
    const u32 saved = gw_load32(0x8219E850);
    gw_store32(0x8219E850, kHashT);
    int differ = 0;
    for (int n = 0; n < 10; ++n) {
      random_entities(200, 520);
      make_hash(0);
      // Each entity's node at +104, linked in its (or a stale) cell.
      for (u32 i = 0; i < 200; ++i) {
        const u32 e = kEntsT + i * kEntSz, node = e + 104;
        float x = lf(e + 4), y = lf(e + 8);
        if (rand() % 3 == 0) {  // moved since: it sits in its old cell
          x += frand(-60, 60);
          y += frand(-60, 60);
        }
        const u32 c = ((u32)((i32)x >> 5) & 0x1F) | ((u32)(i32)y & 0x3E0);
        const u32 cellp = kHashT + c * 16;
        gw_store32(node + 0, e);
        gw_store32(node + 12, c);
        gw_store32(node + 4, cellp);
        gw_store32(node + 8, gw_load32(cellp + 8));
        gw_store32(gw_load32(cellp + 8) + 4, node);
        gw_store32(cellp + 8, node);
        // Its handle: object {generation, pointer}, the target at pointer +
        // offset: a 64-byte object with +60 a value and its own handle at
        // +4 (object), +8 (generation), +10 (offset).
        const u32 h = kHandlesT + i * 8, h2 = kHandlesT + 0x800 + i * 8;
        const u32 target = kHandlesT + 0x1000 + i * 64, gen = rand() & 0xFFFF, gen2 = rand() & 0xFFFF;
        const u32 offset = (rand() % 4) * 16;
        gw_store16(h, gen);
        gw_store32(h + 4, rand() % 8 ? target - offset : 0);
        gw_store32(e + 120, h);
        gw_store16(e + 124, rand() % 6 ? gen : gen ^ 1);
        gw_store16(e + 126, offset);
        gw_store32(target + 4, h2);
        gw_store16(target + 8, rand() % 5 ? gen2 : gen2 ^ 4);
        gw_store16(target + 10, rand() % 2);
        sf(target + 60, frand(-1, 1));
        gw_store16(h2, gen2);
        gw_store32(h2 + 4, rand() % 6 ? kEntsT : (rand() & 1 ? 0 : 0xFFFFFFFF));
      }
      gw_store8(0x8219E84C, (u8)(n & 1));
      for (u32 i = 0; i < 200; ++i) {
        const u32 e = kEntsT + i * kEntSz;
        const bool ok = twice(T, kArea,
            [&](int pass, PPCContext& ctx) {
              ctx.r3.u64 = e;
              if (pass == 0) original_sub_82046BD8(ctx, gw_host(0));
              else sub_82046BD8(ctx, gw_host(0));
            },
            [&](PPCContext&) { return Regs{}; });
        if (!ok) ++differ;
        // The native pass's result is the next call's input (as in a frame).
      }
    }
    gw_store32(0x8219E850, saved);
    printf("hash cell and handle flags (0x82046BD8): %d of 2000 differ%s\n", differ, speed());
    if (differ) ++bad;
  return bad;
}

  // Grid pushers (0x820631E8, and 0x8205A2B0 on its own).
__declspec(noinline) int check_enemy_3() {
  int bad = 0;
    int differ = 0, full = 0;
    const u32 table = kPushObj + 16;
    for (int n = 0; n < 200; ++n) {
      const u32 count = 20 + rand() % 60;
      random_entities(count, 700);
      const bool big = n % 4 == 0;  // big circles, nearly all pushing: fills the table
      if (big)
        for (u32 i = 0; i < count; ++i) sf(kEntsT + i * kEntSz + 32, frand(200, 900));
      for (u32 i = 0; i < count; ++i) {
        u32 f = gw_load32(kEntsT + i * kEntSz + 36);
        if (big || rand() % 3) f &= ~0x500031u;
        gw_store32(kEntsT + i * kEntSz + 36, f);
      }
      u32 ents[100];
      for (u32 i = 0; i < count; ++i) ents[i] = kEntsT + i * kEntSz;
      const u32 split = count / 2;
      make_list(0x8218139C, kNodesT, ents, split);
      make_list(0x821813B4, kNodesT + 0x1000, ents + split, count - split);
      gw_store32(table + 12, rand());
      gw_store32(table + 16, kPushEntries);
      bool was_full = false;
      const bool ok = twice(T, 0x58100,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = kPushObj;
            if (pass == 0) original_sub_820631E8(ctx, gw_host(0));
            else sub_820631E8(ctx, gw_host(0));
            was_full = gw_load32(table + 12) >= 4096;
          },
          [&](PPCContext&) { return Regs{}; });
      if (!ok) ++differ;
      full += was_full;
      // One circle at a time, with double-precision radius and strength.
      for (int k = 0; k < 20; ++k) {
        gw_store32(table + 12, k == 19 ? 4096 - rand() % 30 : rand() % 4096);
        sf(kMisc, frand(-700, 700));
        sf(kMisc + 4, frand(-400, 400));
        const double r = (double)frand(-5, 300) + (k & 1 ? 1e-7 : 0), s = (double)frand(0, 50) + 1e-9;
        const bool ok2 = twice(kPushEntries, 0x18100,
            [&](int pass, PPCContext& ctx) {
              ctx.r3.u64 = table;
              ctx.r4.u64 = kMisc;
              ctx.f1.f64 = r;
              ctx.f2.f64 = s;
              if (pass == 0) original_sub_8205A2B0(ctx, gw_host(0));
              else sub_8205A2B0(ctx, gw_host(0));
            },
            [&](PPCContext& ctx) { return Regs{{ctx.r3.u64 & 0xFF, 0, 0, 0}}; });
        if (!ok2) ++differ;
      }
    }
    printf("grid pushers (0x820631E8, 0x8205A2B0): %d of 4200 differ (%d frames filled the table)%s\n", differ, full, speed());
    if (differ) ++bad;
  return bad;
}

  // Arena and box clamps (0x8205C5D8, 0x8205C6D8).
__declspec(noinline) int check_enemy_4() {
  int bad = 0;
    int differ = 0;
    for (int n = 0; n < 40000; ++n) {
      const u32 pos = kMisc, out = kMisc + 16, lo = kMisc + 32, hi = kMisc + 40;
      sf(pos, frand(-700, 700));
      sf(pos + 4, frand(-420, 420));
      sf(out, 123.0f);
      sf(out + 4, -7.0f);
      sf(lo, frand(-600, -100));
      sf(lo + 4, frand(-350, -50));
      sf(hi, frand(100, 600));
      sf(hi + 4, frand(50, 350));
      const double r = (double)frand(0, 60);
      const bool box = n & 1;
      const bool ok = twice(kMisc, 0x1000,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = pos;
            ctx.r4.u64 = out;
            ctx.r6.u64 = lo;
            ctx.r7.u64 = hi;
            ctx.f1.f64 = r;
            if (box) pass == 0 ? original_sub_8205C6D8(ctx, gw_host(0)) : sub_8205C6D8(ctx, gw_host(0));
            else pass == 0 ? original_sub_8205C5D8(ctx, gw_host(0)) : sub_8205C5D8(ctx, gw_host(0));
          },
          [&](PPCContext& ctx) { return Regs{{ctx.r3.u64, 0, 0, 0}}; });
      if (!ok) ++differ;
    }
    printf("arena and box clamps (0x8205C5D8, 0x8205C6D8): %d of 40000 differ%s\n", differ, speed());
    if (differ) ++bad;
  return bad;
}

  // Entity::Tick (0x820453E0): random flags over the bits it reads, the
  // arena table entries, players to touch.
__declspec(noinline) int check_enemy_5() {
  int bad = 0;
    int differ = 0;
    for (int n = 0; n < 60; ++n) {
      random_entities(120, 700);
      const u32 bits = 0x10 | 0x2 | 0x8 | 0x800 | 0x800000 | 0x1 | 0x4 | 0x40 | 0x100 | 0x200 | 0xA;
      for (u32 i = 0; i < 120; ++i) {
        const u32 e = kEntsT + i * kEntSz;
        u32 f = ((u32)rand() << 12 ^ (u32)rand()) & bits;
        if (rand() % 3 == 0) f = (f & ~0x144u) | 0x104;  // touches players
        gw_store32(e + 36, f);
        gw_store32(e + 168, rand() % 4);
      }
      for (u32 a = 0; a < 4; ++a) {
        const u32 arena = 0x8219CEB4 + a * 108;
        gw_store8(arena + 68, (u8)(a & 1));
        sf(arena + 72, frand(-600, -200));
        sf(arena + 76, frand(-350, -100));
        sf(arena + 80, frand(200, 600));
        sf(arena + 84, frand(100, 350));
      }
      gw_store8(0x8219E7BC, (u8)(n % 3 == 0));
      // Players: some of the entities (bit 0 random).
      u32 players[12];
      for (u32 i = 0; i < 12; ++i) players[i] = kEntsT + (rand() % 120) * kEntSz;
      make_list(0x821813A8, kNodesT, players, 12);
      const bool ok = twice(T, kArea,
          [&](int pass, PPCContext& ctx) {
            for (u32 i = 0; i < 120; ++i) {
              ctx.r1.u64 = kSp;
              ctx.r3.u64 = kEntsT + i * kEntSz;
              if (pass == 0) original_sub_820453E0(ctx, gw_host(0));
              else sub_820453E0(ctx, gw_host(0));
              enemy_log((u32)ctx.r1.u64);
              enemy_log((u32)ctx.lr);
            }
          },
          [&](PPCContext&) { return Regs{}; });
      if (!ok) ++differ;
    }
    printf("Entity::Tick (0x820453E0): %d of 60 crowds differ (log of %u calls)%s\n", differ, g_enemy_log_n, speed());
    if (differ) ++bad;
  return bad;
}

  // Look-at matrix (0x82047180), scaled matrix (0x82040F28).
__declspec(noinline) int check_enemy_6() {
  int bad = 0;
    int differ = 0;
    for (int n = 0; n < 40000; ++n) {
      for (u32 k = 0; k < 16; ++k) sf(kMisc + 0x100 + k * 4, frand(-3, 3));  // position, forward, up, scale
      for (u32 k = 0; k < 16; ++k) sf(kMisc + 0x200 + k * 4, frand(-3, 3));  // a matrix
      if (n % 50 == 0) sf(kMisc + 0x110, 0.0f), sf(kMisc + 0x114, 0.0f), sf(kMisc + 0x118, 0.0f), sf(kMisc + 0x11C, 0.0f);
      const bool in_place = n & 1;
      const bool ok = twice(kMisc, 0x1000,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = kMisc;
            ctx.r4.u64 = kMisc + 0x100;
            ctx.r5.u64 = kMisc + 0x110;
            ctx.r6.u64 = kMisc + 0x120;
            if (pass == 0) original_sub_82047180(ctx, gw_host(0));
            else sub_82047180(ctx, gw_host(0));
            ctx.r3.u64 = in_place ? kMisc + 0x200 : kMisc + 0x300;
            ctx.r4.u64 = kMisc + 0x200;
            ctx.r5.u64 = kMisc + 0x130;
            if (pass == 0) original_sub_82040F28(ctx, gw_host(0));
            else sub_82040F28(ctx, gw_host(0));
          },
          [&](PPCContext&) { return Regs{}; });
      if (!ok) ++differ;
    }
    printf("look-at and scaled matrices (0x82047180, 0x82040F28): %d of 40000 differ%s\n", differ, speed());
    if (differ) ++bad;
  return bad;
}

  // Nearest player (0x8204F458).
__declspec(noinline) int check_enemy_7() {
  int bad = 0;
    int differ = 0;
    for (int n = 0; n < 2000; ++n) {
      random_entities(40, 600);
      u32 players[10];
      const u32 np = rand() % 10;
      for (u32 i = 0; i < np; ++i) players[i] = kEntsT + (rand() % 40) * kEntSz;
      make_list(0x821813A8, kNodesT, players, np);
      const u32 self = kEntsT + (rand() % 40) * kEntSz;
      const bool ok = twice(T, kArea,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = self;
            if (pass == 0) original_sub_8204F458(ctx, gw_host(0));
            else sub_8204F458(ctx, gw_host(0));
          },
          [&](PPCContext& ctx) { return Regs{{ctx.r3.u64, 0, 0, 0}}; });
      if (!ok) ++differ;
    }
    printf("nearest player (0x8204F458): %d of 2000 differ%s\n", differ, speed());
    if (differ) ++bad;
  return bad;
}

  // Enemy updates: the homing one (0x82056CD8), the wanderer (0x82059838,
  // with the player choice 0x82045AB8 and separation), the pulsing chaser
  // (0x82049E60), and the world matrix (0x82056B48).
__declspec(noinline) int check_enemy_8() {
  int bad = 0;
    const u32 saved = gw_load32(0x8219E850), saved2 = gw_load32(0x8219F9AC);
    gw_store32(0x8219E850, kHashT);
    gw_store32(0x8219F9AC, kHashT);
    int differ[8] = {};
    for (int n = 0; n < 40; ++n) {
      random_entities(120, 500);
      for (u32 i = 0; i < 120; ++i) {
        const u32 e = kEntsT + i * kEntSz;
        gw_store32(e + 68, rand() % 4 == 0 ? 13 : rand() % 20);
        gw_store32(e + 72, rand() % 3 ? kEntsT + (rand() % 120) * kEntSz : 0);
        gw_store32(e + 160, rand() % 3 == 0 ? kEntsT + (rand() % 120) * kEntSz : 0);
        sf(e + 20, frand(-4, 4));
        sf(e + 24, frand(0.5f, 2));
        sf(e + 28, frand(-2, 2));
        sf(e + 128, frand(-0.3f, 0.3f));
        sf(e + 132, frand(-10, 10));
        sf(e + 140, frand(-0.1f, 0.2f));
        sf(e + 144, frand(-0.1f, 1.3f));
        sf(e + 148, frand(0, 2));
        gw_store8(e + 152, rand() & 1);
        if (rand() % 7 == 0) {  // on top of its aim: the distance 1 test, the 0 length
          const u32 a = gw_load32(e + 160) ? gw_load32(e + 160) : gw_load32(e + 72);
          if (a) {
            sf(e + 4, lf(a + 4) + frand(-0.5f, 0.5f) * (rand() & 1));
            sf(e + 8, lf(a + 8));
          }
        }
      }
      make_hash(120);
      for (u32 k = 0; k < 4; ++k) {
        sf(0x821A0058 + k * 4, frand(0, 1));
        sf(0x821A059C + k * 4, frand(0, 1));
      }
      u32 players[6];
      const u32 np = rand() % 5;
      for (u32 i = 0; i < np; ++i) players[i] = kEntsT + (rand() % 120) * kEntSz;
      make_list(0x821813A8, kNodesT, players, np);
      for (int which = 0; which < 8; ++which) {
        if (which == 7) {  // the drifter's fields, its constants
          for (u32 k = 0; k < 8; ++k) sf(0x8219FD40 + k * 4, frand(0.1f, 1.5f));
          sf(0x8219FE10, frand(0.05f, 0.5f));
          sf(0x8219FE44, frand(0.02f, 0.3f));
          sf(0x8219FDA8, frand(0.8f, 1));
          sf(0x8219FD74, frand(0.8f, 1));
          sf(0x8219FDDC, frand(1, 40));
          for (u32 i = 0; i < 120; ++i) {
            const u32 e = kEntsT + i * kEntSz;
            sf(e + 128, frand(-8, 8));
            sf(e + 132, frand(-7, 7));
            gw_store8(e + 136, rand() % 3 == 0);
            sf(e + 140, rand() % 4 ? frand(-3.2f, 3.2f) : frand(-0.02f, 0.02f));
            sf(e + 144, frand(-3.2f, 3.2f));
          }
        }
        if (which == 6) {  // the roller's period and fields (last: +140..+176 are others' pointers)
          sf(0x8219E788, frand(0.3f, 1.2f));
          for (u32 i = 0; i < 120; ++i) {
            const u32 e = kEntsT + i * kEntSz;
            sf(e + 128, frand(0, 1.3f));
            for (u32 k = 136; k <= 168; k += 4) sf(e + k, frand(-4, 4));
            gw_store32(e + 172, rand() % 4);
            gw_store8(e + 176, rand() & 1);
          }
        }
        const bool ok = twice(T, kArea,
            [&](int pass, PPCContext& ctx) {
              g_random_calls = 0;
              for (u32 i = 0; i < 120; ++i) {
                ctx.r1.u64 = kSp;
                ctx.lr = 0x12345678;
                ctx.r3.u64 = kEntsT + i * kEntSz;
                if (which == 0) pass == 0 ? original_sub_82056CD8(ctx, gw_host(0)) : sub_82056CD8(ctx, gw_host(0));
                if (which == 1) pass == 0 ? original_sub_82059838(ctx, gw_host(0)) : sub_82059838(ctx, gw_host(0));
                if (which == 2) pass == 0 ? original_sub_82049E60(ctx, gw_host(0)) : sub_82049E60(ctx, gw_host(0));
                if (which == 3) {
                  ctx.r3.u64 = kMisc + 0x400 + (i % 8) * 64;
                  ctx.r4.u64 = kEntsT + i * kEntSz;
                  pass == 0 ? original_sub_82056B48(ctx, gw_host(0)) : sub_82056B48(ctx, gw_host(0));
                  enemy_log(ctx.r3.u32);
                  for (u32 k = 0; k < 64; k += 4) enemy_log(gw_load32(kMisc + 0x400 + (i % 8) * 64 + k));
                }
                if (which == 7) pass == 0 ? original_sub_82054E30(ctx, gw_host(0)) : sub_82054E30(ctx, gw_host(0));
                if (which == 6) pass == 0 ? original_sub_82044330(ctx, gw_host(0)) : sub_82044330(ctx, gw_host(0));
                if (which == 5) pass == 0 ? original_sub_82059CD0(ctx, gw_host(0)) : sub_82059CD0(ctx, gw_host(0));
                if (which == 4) {
                  pass == 0 ? original_sub_82045AB8(ctx, gw_host(0)) : sub_82045AB8(ctx, gw_host(0));
                  enemy_log(ctx.r3.u32);
                }
                enemy_log(ctx.r1.u32);
                enemy_log((u32)ctx.lr);
              }
              enemy_log(g_random_calls);
            },
            [&](PPCContext&) { return Regs{}; });
        if (!ok) ++differ[which];
      }
    }
    gw_store32(0x8219E850, saved);
    gw_store32(0x8219F9AC, saved2);
    printf("enemy updates: homing (0x82056CD8) %d, wanderer (0x82059838) %d, pulsing (0x82049E60) %d,\n"
           "  world matrix (0x82056B48) %d, player choice (0x82045AB8) %d, dodger (0x82059CD0) %d,\n"
           "  four-way roller (0x82044330) %d, drifter (0x82054E30) %d of 40 crowds differ%s\n",
           differ[0], differ[1], differ[2], differ[3], differ[4], differ[5], differ[6], differ[7], speed());
    for (int d : differ) bad += d != 0;
  return bad;
}

  // Vertex builders: the glowing 3D line (0x82060CC0, its two streams at
  // kLineQuad/kLineOutline), the glow sprite (0x8205FA98) and an entity's
  // glow (0x82045B18, through its handle); the vertex buffers compared.
__declspec(noinline) int check_enemy_9() {
  int bad = 0;
    int differ[3] = {};
    for (int n = 0; n < 20000; ++n) {
      const u32 a = kMisc, b = kMisc + 16, col = kMisc + 32;
      for (u32 k = 0; k < 3; ++k) sf(a + k * 4, frand(-600, 600));
      for (u32 k = 0; k < 3; ++k) sf(b + k * 4, n % 50 == 0 ? lf(a + k * 4) : frand(-600, 600));
      for (u32 k = 0; k < 4; ++k) sf(col + k * 4, frand(0, 1));
      sf(0x821A1DB4, frand(0.5f, 3));
      const double w = (double)frand(0.1f, 8) + (n & 1 ? 1e-9 : 0);
      const bool ok = twice(kLineQuad, 0x2000,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = a;
            ctx.r4.u64 = b;
            ctx.r5.u64 = col;
            ctx.f1.f64 = w;
            pass == 0 ? original_sub_82060CC0(ctx, gw_host(0)) : sub_82060CC0(ctx, gw_host(0));
          },
          [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, 0, 0, 0}}; });
      if (!ok) ++differ[0];
      const bool ok2 = twice(kSpriteVerts, 0x1000,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = a;
            ctx.r5.u64 = col;
            ctx.f1.f64 = w;
            pass == 0 ? original_sub_8205FA98(ctx, gw_host(0)) : sub_8205FA98(ctx, gw_host(0));
          },
          [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, 0, 0, 0}}; });
      if (!ok2) ++differ[1];
    }
    // The glow: entities with live or dead handles, some fading.
    random_entities(100, 500);
    for (u32 i = 0; i < 100; ++i) {
      const u32 e = kEntsT + i * kEntSz, h = kHandlesT + i * 8, owner = kHandlesT + 0x1000 + i * 64;
      const u32 gen = rand() & 0xFFFF;
      gw_store16(h, gen);
      gw_store32(h + 4, rand() % 8 ? owner : 0);
      gw_store32(e + 120, h);
      gw_store16(e + 124, rand() % 6 ? gen : gen ^ 2);
      gw_store16(e + 126, 0);
      for (u32 k = 44; k <= 60; k += 4) sf(owner + k, frand(0, 2));
      gw_store32(e + 36, rand() & 1 ? 0x10 : 0x2);
    }
    for (int n = 0; n < 100; ++n) {
      const bool ok = twice(kSpriteVerts, 0x1000,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = kEntsT + n * kEntSz;
            pass == 0 ? original_sub_82045B18(ctx, gw_host(0)) : sub_82045B18(ctx, gw_host(0));
          },
          [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, 0, 0, 0}}; });
      if (!ok) ++differ[2];
    }
    printf("vertex builders: 3D line (0x82060CC0) %d, sprite (0x8205FA98) %d of 20000, glow (0x82045B18) %d of 100 differ%s\n",
           differ[0], differ[1], differ[2], speed());
    for (int d : differ) bad += d != 0;
  return bad;
}

  // The roller's world matrix (0x82044168): random angles and positions.
__declspec(noinline) int check_enemy_10() {
  int bad = 0;
    int differ = 0;
    for (int n = 0; n < 20000; ++n) {
      const u32 e = kMisc + 0x100;
      sf(e + 4, frand(-600, 600));
      sf(e + 8, frand(-400, 400));
      sf(e + 140, frand(-7, 7));
      sf(e + 152, frand(-7, 7));
      const bool ok = twice(kMisc, 0x100,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = kMisc;
            ctx.r4.u64 = e;
            pass == 0 ? original_sub_82044168(ctx, gw_host(0)) : sub_82044168(ctx, gw_host(0));
          },
          [&](PPCContext& ctx) { return Regs{{ctx.r3.u64, ctx.r1.u64, 0, 0}}; });
      if (!ok) ++differ;
    }
    printf("roller world matrix (0x82044168): %d of 20000 differ%s\n", differ, speed());
    if (differ) ++bad;
  return bad;
}

  // Line debris (0x82074B28): pieces near and past the screen's edges,
  // some running out (removed by copying the last one over them).
__declspec(noinline) int check_enemy_11() {
  int bad = 0;
    int differ = 0;
    const u32 set = kMisc, pieces = T + 0x10000;
    for (u32 k = 0; k < 8; ++k) sf(0x821B470C + k * 4, frand(0.01f, 2));
    for (u32 k = 0; k < 8; ++k) sf(0x821B4740 + k * 4, frand(0.01f, 2));
    for (int n = 0; n < 400; ++n) {
      const u32 count = rand() % 200;
      for (u32 i = 0; i < count; ++i) {
        const u32 p = pieces + i * 92;
        for (u32 k = 0; k < 92; k += 4) sf(p + k, frand(-8, 8));
        sf(p + 0, frand(-700, 700));
        sf(p + 4, frand(-400, 400));
        sf(p + 8, lf(p + 0) + frand(-30, 30));
        sf(p + 12, lf(p + 4) + frand(-30, 30));
        sf(p + 64, frand(1, 40));
        sf(p + 68, rand() % 5 == 0 ? frand(-0.1f, 0.01f) : frand(0, 2));
      }
      gw_store32(set + 0, pieces);
      gw_store32(set + 4, count);
      const bool ok = twice(T, kArea,
          [&](int pass, PPCContext& ctx) {
            ctx.r3.u64 = n % 37 == 0 ? 0 : set;
            pass == 0 ? original_sub_82074B28(ctx, gw_host(0)) : sub_82074B28(ctx, gw_host(0));
          },
          [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, gw_load32(set + 4), 0, 0}}; });
      if (!ok) ++differ;
    }
    printf("line debris (0x82074B28): %d of 400 differ%s\n", differ, speed());
    if (differ) ++bad;
  return bad;
}

  // Entity drawing (0x82045740 with 0x82045628, 0x82061908, 0x82061A30):
  // random flags, kinds, outlines; the line buffers and the call log.
__declspec(noinline) int check_enemy_12() {
  int bad = 0;
  // (The hash-cell case's handle targets share this memory with the vtable.)
  gw_store32(kVtbl + 4, kFakeDraw4);
  gw_store32(kVtbl + 8, kFakeDraw8);
    int differ = 0;
    const u32 segs = T + 0x30000;
    for (u32 k = 0; k < 64; ++k) sf(segs + k * 4, frand(-3, 3));
    for (int n = 0; n < 60; ++n) {
      random_entities(30, 600);
      for (u32 i = 0; i < 30; ++i) {
        const u32 e = kEntsT + i * kEntSz;
        const u32 pick[] = {0, 0, 0x10, 0x10, 0x40010, 0x400, 0x1000000, 0x1000010, 0x1000400};
        gw_store32(e + 36, pick[rand() % 9]);
        gw_store32(e + 68, rand() % 3 ? 34 : rand() % 40);
        sf(e + 20, frand(-7, 7));
        sf(e + 24, frand(0.2f, 3));
        sf(e + 28, frand(0.2f, 3));
        for (u32 k = 40; k <= 52; k += 4) sf(e + k, frand(0, 1));
        sf(e + 56, frand(0, 1.5f));
        gw_store32(e + 60, rand() % 10 - (rand() % 9 == 0 ? 3 : 0));
        gw_store32(e + 64, rand() % 9 ? segs + (rand() % 4) * 16 : 0);
      }
      // The glowing line's two streams (src/shapes_native.cpp's): chunks
      // in kVerts, one vertex used already (so nothing is deferred).
      const bool ok = twice(kVerts, kTSize,
          [&](int pass, PPCContext& ctx) {
            gw_store32(0x821A0000 + 12424 + 2052, kVerts);
            gw_store32(0x821A0000 + 12424 + 2056, 1);
            gw_store32(0x821A0000 + 14536 + 2052, kVerts + 0x40000);
            gw_store32(0x821A0000 + 14536 + 2056, 1);
            for (u32 i = 0; i < 30; ++i) {
              ctx.r1.u64 = kSp;
              ctx.r3.u64 = kEntsT + i * kEntSz;
              pass == 0 ? original_sub_82045740(ctx, gw_host(0)) : sub_82045740(ctx, gw_host(0));
              enemy_log(ctx.r1.u32);
              enemy_log((u32)ctx.lr);
            }
            enemy_log(gw_load32(0x821A0000 + 12424 + 2056));
            enemy_log(gw_load32(0x821A0000 + 14536 + 2056));
          },
          [&](PPCContext&) { return Regs{}; });
      if (!ok) ++differ;
    }
    printf("entity drawing (0x82045740, 0x82045628, 0x82061908, 0x82061A30): %d of 60 crowds differ (log of %u)%s\n",
           differ, g_enemy_log_n, speed());
    if (differ) ++bad;
  return bad;
}
// atan2 (0x8212FE70): every quadrant, the axes, signed zeros, infinities,
// NaNs (quiet and signalling, with payloads), tiny and huge values, the
// octant folds' edges, and random pairs; f1 compared bit for bit.
__declspec(noinline) int check_atan2() {
  const double special[] = {0.0, -0.0, 1.0, -1.0, 0.5, -2.0, 1e-310, -1e-310, 2.2250738585072014e-308,
                            1e300, -1e300, 1.7976931348623157e308, __builtin_inf(), -__builtin_inf(),
                            __builtin_nan(""), -__builtin_nan(""), __builtin_nans(""), 0.2679491924311227,
                            0.26794919243112275, 1.7320508075688772, 3.0, -3.0, 1e-20, -1e-20};
  constexpr int kSpecial = sizeof(special) / sizeof(special[0]);
  int differ = 0, n = 0;
  auto one = [&](double y, double x) {
    static PPCContext a, b;
    a = PPCContext{};
    a.r1.u64 = 0x70100000;
    a.f1.f64 = y;
    a.f2.f64 = x;
    b = a;
    original_sub_8212FE70(a, gw_host(0));
    sub_8212FE70(b, gw_host(0));
    ++n;
    if (a.f1.u64 != b.f1.u64) {
      if (differ < 5) printf("    atan2(%.17g, %.17g): original %016llX native %016llX\n", y, x,
                             (unsigned long long)a.f1.u64, (unsigned long long)b.f1.u64);
      ++differ;
    }
  };
  for (int i = 0; i < kSpecial; ++i)
    for (int j = 0; j < kSpecial; ++j) one(special[i], special[j]);
  // NaN payloads.
  for (int k = 0; k < 64; ++k) {
    u64 bits = 0x7FF0000000000001ull + ((u64)rand() << 20) + (u64)k, bits2 = bits ^ 0x8000000000000000ull;
    double nan1, nan2;
    memcpy(&nan1, &bits, 8);
    memcpy(&nan2, &bits2, 8);
    one(nan1, 1.0), one(1.0, nan1), one(nan1, nan2), one(nan2, -0.0), one(0.0, nan2);
  }
  // Random pairs over many scales, and around the 2 - sqrt 3 fold and the diagonal.
  for (int k = 0; k < 400000; ++k) {
    const double ys = ldexp(1.0, rand() % 80 - 40), xs = ldexp(1.0, rand() % 80 - 40);
    double y = (double)frand(-1, 1) * ys, x = (double)frand(-1, 1) * xs;
    if (k % 7 == 0) x = y * (k & 8 ? 1 : -1) * (1.0 + (double)frand(-1e-6f, 1e-6f));
    if (k % 11 == 0) y = x * 0.2679491924311227 * (1.0 + (double)frand(-1e-7f, 1e-7f));
    one(y, x);
  }
  printf("atan2 (0x8212FE70): %d of %d differ\n", differ, n);
  return differ ? 1 : 0;
}

// The shifting colour (0x82030678): random clocks, scales, phases, colours.
__declspec(noinline) int check_colour() {
  int differ = 0;
  for (int n = 0; n < 40000; ++n) {
    sf(0x82199260, frand(-2000, 2000));
    sf(0x82199344, frand(0, 1));
    sf(0x82199378, frand(0, 1));
    for (u32 k = 0; k < 4; ++k) sf(kMisc + 0x100 + k * 4, frand(0, 1.5f));
    const double phase = (double)frand(-30, 30);
    const bool ok = twice(kMisc, 0x200,
        [&](int pass, PPCContext& ctx) {
          ctx.r3.u64 = kMisc;
          ctx.r4.u64 = kMisc + 0x100;
          ctx.f1.f64 = phase;
          pass == 0 ? original_sub_82030678(ctx, gw_host(0)) : sub_82030678(ctx, gw_host(0));
        },
        [&](PPCContext& ctx) { return Regs{{ctx.r3.u64, ctx.r1.u64, 0, 0}}; });
    if (!ok) ++differ;
  }
  printf("shifting colour (0x82030678): %d of 40000 differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// The scene draw (0x82065E38): its nine streams, the vertex-buffer ring, the
// eight draw lists (entities drawn by their virtual +8, some dead); the
// streams' words and list entries, the ring and the call log compared.
__declspec(noinline) int check_scene_draw() {
  int differ = 0;
  const u32 streams[] = {0x821A0000 + 27016, 0x821A0000 + 12424, 0x821A0000 + 14536, 0x821B0000 - 28200,
                         0x821B0000 - 26136, 0x821A0000 + 29080, 0x821A0000 + 31144, 0x821B0000 - 32328,
                         0x821B0000 - 30264};
  const u32 lists[] = {5068, 5020, 5080, 5044, 5056, 5032, 5200};
  gw_store32(kVtbl + 8, kFakeDraw8);
  for (int n = 0; n < 200; ++n) {
    random_entities(40, 600);
    for (u32 i = 0; i < 40; ++i) gw_store32(kEntsT + i * kEntSz + 36, 0x400 | (rand() % 4 == 0 ? (u32)(rand() & 3) : 0));
    for (u32 l = 0; l < 7; ++l) {
      u32 ents[12];
      const u32 count = rand() % 12;
      for (u32 i = 0; i < count; ++i) ents[i] = kEntsT + (rand() % 40) * kEntSz;
      make_list(0x82180000 + lists[l], kNodesT + l * 0x100, ents, count);
    }
    for (u32 k = 0; k < 256; ++k) gw_store32(0x821B0000 + 16672 + k * 4, 0x71000000 + k * 0x1000 + n);
    gw_store8(0x821B0000 + 14688 + 32, n & 1);
    const u32 ring = rand() % 256;
    const bool ok = twice(kMisc, 0x100,
        [&](int pass, PPCContext& ctx) {
          gw_store32(0x821B0000 + 16664, ring);
          u32 k = 0;
          for (u32 st : streams) {  // the same in both passes
            gw_store32(st + 2048, (u32)(n * 7 + k++ * 3) % 5);
            gw_store32(st + 2056, 77);
          }
          gw_store32(0x8219D290, kMisc + 0x800);  // a device the camera (0x82067A20) can write
          pass == 0 ? original_sub_82065E38(ctx, gw_host(0)) : sub_82065E38(ctx, gw_host(0));
          for (u32 st : streams) {
            const u32 len = gw_load32(st + 2048);
            enemy_log(len);
            enemy_log(gw_load32(st + 2052));
            enemy_log(gw_load32(st + 2056));
            for (u32 k = 0; k < len && k < 8; ++k) enemy_log(gw_load32(st + k * 8)), enemy_log(gw_load32(st + k * 8 + 4));
          }
          enemy_log(gw_load32(0x821B0000 + 16664));
        },
        [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, ctx.r3.u64, 0, 0}}; });
    if (!ok) ++differ;
  }
  printf("scene draw (0x82065E38): %d of 200 differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// The two-colour 3D line (0x8205F6B8): random ends, colours, widths (and
// zero-length lines); its glow block at kLineQuad, its outline at kLineOutline.
__declspec(noinline) int check_line2() {
  int differ = 0;
  g_alloc_stream = 0x821B0000 - 32328;
  g_alloc_buffer = kLineQuad;
  for (int n = 0; n < 20000; ++n) {
    const u32 a = kMisc, b = kMisc + 16, c0 = kMisc + 32, c1 = kMisc + 48;
    for (u32 k = 0; k < 3; ++k) sf(a + k * 4, frand(-600, 600));
    for (u32 k = 0; k < 3; ++k) sf(b + k * 4, n % 50 == 0 ? lf(a + k * 4) : frand(-600, 600));
    for (u32 k = 0; k < 4; ++k) sf(c0 + k * 4, frand(0, 1)), sf(c1 + k * 4, frand(0, 1));
    sf(0x821A1DB4, frand(0.5f, 3));
    const double w = (double)frand(0.1f, 8) + (n & 1 ? 1e-9 : 0);
    const bool ok = twice(kLineQuad, 0x2000,
        [&](int pass, PPCContext& ctx) {
          ctx.r3.u64 = a;
          ctx.r4.u64 = b;
          ctx.r5.u64 = c0;
          ctx.r6.u64 = c1;
          ctx.f1.f64 = w;
          pass == 0 ? original_sub_8205F6B8(ctx, gw_host(0)) : sub_8205F6B8(ctx, gw_host(0));
        },
        [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, 0, 0, 0}}; });
    if (!ok) ++differ;
  }
  g_alloc_stream = 0;
  printf("two-colour 3D line (0x8205F6B8): %d of 20000 differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// The blinking entity (0x8204D8E0): ages around its life and last second,
// with and without an owner, both blink states, fast and slow.
__declspec(noinline) int check_blinker() {
  int differ = 0;
  for (int n = 0; n < 200; ++n) {
    random_entities(60, 500);
    for (u32 i = 0; i < 60; ++i) {
      const u32 e = kEntsT + i * kEntSz;
      const float life = frand(1, 5);
      sf(e + 116, life);
      sf(e + 104, life + frand(-1.5f, 0.3f));
      gw_store32(e + 72, rand() % 3 == 0 ? kEntsT : 0);
      gw_store8(e + 112, rand() & 1);
      for (u32 k = 120; k <= 132; k += 4) sf(e + k, frand(0, 1));
      if (rand() % 3 == 0) sf(e + 12, frand(-60, 60)), sf(e + 16, frand(-60, 60));
      sf(e + 20, frand(-7, 7));
    }
    const bool ok = twice(T, kArea,
        [&](int pass, PPCContext& ctx) {
          for (u32 i = 0; i < 60; ++i) {
            ctx.r1.u64 = kSp;
            ctx.r3.u64 = kEntsT + i * kEntSz;
            pass == 0 ? original_sub_8204D8E0(ctx, gw_host(0)) : sub_8204D8E0(ctx, gw_host(0));
            enemy_log(ctx.r1.u32);
            enemy_log((u32)ctx.lr);
          }
        },
        [&](PPCContext&) { return Regs{}; });
    if (!ok) ++differ;
  }
  printf("blinking entity (0x8204D8E0): %d of 200 crowds differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// Model drawing (0x82056FA0, 0x82046D88, 0x82047DF8): entities with live and
// dead handles, owners with and without a model, fading or not; the model
// lines (src/shapes_native.cpp's 0x82061DA0) into kVerts, compared with the log.
__declspec(noinline) int check_models() {
  int differ[3] = {};
  const u32 owners = kHandlesT + 0x1000, models = kHandlesT + 0x2000;
  // Four models as 0x82061DA0 reads them: +44 the mesh {+0 parts, +4 part
  // count, +12 the caps' length}, a part {+0 segments, +4 count}, segments
  // 48 bytes (start, end, colour).
  for (u32 k = 0; k < 4 * 64 * 12; ++k) sf(models + 0x800 + k * 4, frand(-3, 3));
  for (u32 m = 0; m < 4; ++m) {
    const u32 mdl = models + m * 64, mesh = models + 0x400 + m * 32, part = models + 0x600 + m * 16;
    gw_store32(mdl + 44, mesh);
    gw_store32(mesh + 0, part);
    gw_store32(mesh + 4, 1);
    sf(mesh + 12, frand(0.5f, 4));
    gw_store32(part + 0, models + 0x800 + m * 0x300);
    gw_store32(part + 4, 2 + m);
  }
  for (int n = 0; n < 60; ++n) {
    random_entities(30, 500);
    for (u32 i = 0; i < 30; ++i) {
      const u32 e = kEntsT + i * kEntSz, h = kHandlesT + i * 8, owner = owners + i * 64, h2 = kHandlesT + 0x800 + i * 8;
      const u32 gen = rand() & 0xFFFF, gen2 = rand() & 0xFFFF;
      gw_store16(h, gen);
      gw_store32(h + 4, rand() % 8 ? owner : 0);
      gw_store32(e + 120, h);
      gw_store16(e + 124, rand() % 6 ? gen : gen ^ 2);
      gw_store16(e + 126, 0);
      gw_store32(owner + 4, h2);
      gw_store16(owner + 8, rand() % 5 ? gen2 : gen2 ^ 1);
      gw_store16(owner + 10, 0);
      gw_store32(owner + 12, 0);  // its matrix mode (0x82045CF0): the rotation
      gw_store16(h2, gen2);
      const u32 mdl = models + (i % 4) * 64;
      gw_store32(h2 + 4, rand() % 6 ? mdl : 0);
      gw_store32(e + 36, rand() & 1 ? 0x10 : 0);
      sf(e + 56, frand(0, 1.5f));
      sf(e + 148, frand(0, 1));
      for (u32 k = 40; k <= 52; k += 4) sf(e + k, frand(0, 1));
    }
    for (int which = 0; which < 3; ++which) {
      const bool ok = twice(kVerts, kTSize,
          [&](int pass, PPCContext& ctx) {
            gw_store32(0x821A0000 + 12424 + 2052, kVerts);
            gw_store32(0x821A0000 + 12424 + 2056, 1);
            gw_store32(0x821A0000 + 14536 + 2052, kVerts + 0x40000);
            gw_store32(0x821A0000 + 14536 + 2056, 1);
            for (u32 i = 0; i < 30; ++i) {
              ctx.r1.u64 = kSp;
              ctx.r3.u64 = kEntsT + i * kEntSz;
              if (which == 0) pass == 0 ? original_sub_82056FA0(ctx, gw_host(0)) : sub_82056FA0(ctx, gw_host(0));
              if (which == 1) pass == 0 ? original_sub_82046D88(ctx, gw_host(0)) : sub_82046D88(ctx, gw_host(0));
              if (which == 2) pass == 0 ? original_sub_82047DF8(ctx, gw_host(0)) : sub_82047DF8(ctx, gw_host(0));
              enemy_log(ctx.r1.u32);
              enemy_log((u32)ctx.lr);
            }
            enemy_log(gw_load32(0x821A0000 + 12424 + 2056));
            enemy_log(gw_load32(0x821A0000 + 14536 + 2056));
          },
          [&](PPCContext&) { return Regs{}; });
      if (!ok) ++differ[which];
    }
  }
  printf("model drawing: 0x82056FA0 %d, 0x82046D88 %d, 0x82047DF8 %d of 60 crowds differ%s\n", differ[0], differ[1],
         differ[2], speed());
  return differ[0] + differ[1] + differ[2] ? 1 : 0;
}

// The entity matrix (0x82045CF0): owners in each mode (0..3 and others),
// players to face, entities moving and still; the out matrices and the log.
__declspec(noinline) int check_entity_matrix() {
  int differ = 0;
  gw_dispatch_add(kFakeMatrix, fake_matrix);
  gw_store32(kVtbl + 24, kFakeMatrix);
  const u32 owners = T + 0x36000;  // (clear of the vtable)
  for (int n = 0; n < 100; ++n) {
    random_entities(40, 500);
    for (u32 i = 0; i < 40; ++i) {
      const u32 e = kEntsT + i * kEntSz, h = kHandlesT + i * 8, owner = owners + i * 64;
      const u32 gen = rand() & 0xFFFF;
      gw_store16(h, gen);
      gw_store32(h + 4, owner);
      gw_store32(e + 120, h);
      gw_store16(e + 124, gen);
      gw_store16(e + 126, 0);
      gw_store32(owner + 12, rand() % 5);
      for (u32 k = 16; k <= 24; k += 4) sf(owner + k, frand(-1, 1));
      sf(e + 20, frand(-7, 7));
      gw_store32(e + 72, rand() % 3 ? 0 : kEntsT);
      gw_store32(e + 0, kVtbl);
    }
    u32 players[4];
    const u32 np = rand() % 4;
    for (u32 i = 0; i < np; ++i) players[i] = kEntsT + (rand() % 40) * kEntSz;
    make_list(0x821813A8, kNodesT, players, np);
    if (n % 10 == 0 && np) sf(players[0] + 4, lf(kEntsT + 4)), sf(players[0] + 8, lf(kEntsT + 8));
    const bool ok = twice(kMisc, 0x1000,
        [&](int pass, PPCContext& ctx) {
          for (u32 i = 0; i < 40; ++i) {
            ctx.r1.u64 = kSp;
            ctx.r3.u64 = kMisc + (i % 16) * 64;
            ctx.r4.u64 = kEntsT + i * kEntSz;
            pass == 0 ? original_sub_82045CF0(ctx, gw_host(0)) : sub_82045CF0(ctx, gw_host(0));
            enemy_log(ctx.r3.u32);
            enemy_log(ctx.r1.u32);
            enemy_log((u32)ctx.lr);
          }
        },
        [&](PPCContext&) { return Regs{}; });
    if (!ok) ++differ;
  }
  printf("entity matrix (0x82045CF0): %d of 100 crowds differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// The explosion burst (0x82030C58): random sizes, counts (none, a few,
// many, negative), colours, clocks; the store saying no now and then.
__declspec(noinline) int check_burst() {
  int differ = 0;
  for (int n = 0; n < 4000; ++n) {
    sf(0x82199260, frand(-2000, 2000));
    sf(0x82199344, frand(0, 1));
    sf(0x82199378, frand(0, 1));
    sf(0x821992DC, frand(0.5f, 3));
    sf(0x821992A8, frand(0.2f, 2));
    for (u32 k = 0; k < 4; ++k) sf(kMisc + 0x200 + k * 4, frand(0, 1.5f));
    const double size = (double)frand(1, 40);
    const u32 count = n % 17 == 0 ? (u32)-(rand() % 50) : (u32)(rand() % (n % 5 == 0 ? 400 : 60));
    g_store_full = n % 9 == 0;
    const bool ok = twice(kMisc, 0x400,
        [&](int pass, PPCContext& ctx) {
          g_random_calls = 0;
          ctx.r3.u64 = kMisc + 0x200;
          ctx.r5.u64 = count;
          ctx.f1.f64 = size;
          pass == 0 ? original_sub_82030C58(ctx, gw_host(0)) : sub_82030C58(ctx, gw_host(0));
        },
        [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, 0, 0, 0}}; });
    if (!ok) ++differ;
  }
  printf("explosion burst (0x82030C58): %d of 4000 differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// The camera (0x82067A20): random eyes, offsets, held views and the hold
// byte, field of view and scale; the stored views, the device's constants
// (at kMisc + 0x800) and the frame's matrices (logged) compared.
__declspec(noinline) int check_camera() {
  int differ = 0;
  const u32 device = kMisc + 0x800;
  for (int n = 0; n < 4000; ++n) {
    for (u32 k = 0; k < 4; ++k) sf(0x821A0000 + 7552 + k * 4, frand(-600, 600));
    for (u32 k = 0; k < 16; ++k) sf(0x82180000 + 9752 + k * 4, frand(-2, 2));
    gw_store8(0x821B0000 + 16228 + 32, n % 3 == 0);
    sf(0x821B0000 + 16300 + 32, frand(30, 90));
    sf(0x82180000 + 9708, frand(0.5f, 2));
    sf(0x821B0000 + 16352 + 32, frand(0.5f, 2));
    sf(0x821B0000 + 16404 + 32, frand(-1, 1));
    sf(0x821B0000 + 16456 + 32, frand(-1, 1));
    gw_store32(0x821A0000 - 11632, device);
    memset(gw_host(device), 0, 0x800);
    const bool ok = twice(kMisc, 0x1000,
        [&](int pass, PPCContext& ctx) {
          ctx.f1.f64 = 1.0;
          pass == 0 ? original_sub_82067A20(ctx, gw_host(0)) : sub_82067A20(ctx, gw_host(0));
          for (u32 k = 0; k < 64; k += 4) enemy_log(gw_load32(0x82180000 + 9752 + k)), enemy_log(gw_load32(0x82180000 + 9816 + k));
          const u32 f = kSp - 432;
          for (u32 k = 0; k < 64; k += 4)
            enemy_log(gw_load32(f + 80 + k)), enemy_log(gw_load32(f + 192 + k)), enemy_log(gw_load32(f + 256 + k));
        },
        [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, 0, 0, 0}}; });
    if (!ok) ++differ;
  }
  printf("camera (0x82067A20): %d of 4000 differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// The second line style (0x8205EF18): random ends, colours, thicknesses
// (and zero-length lines); its glow block at kLineQuad, its outline at
// kLineOutline.
__declspec(noinline) int check_line_style2() {
  int differ = 0;
  g_alloc_stream = 0x821B0000 - 28200;
  g_alloc_buffer = kLineQuad;
  for (int n = 0; n < 20000; ++n) {
    const u32 a = kMisc, b = kMisc + 16, c0 = kMisc + 32;
    for (u32 k = 0; k < 4; ++k) sf(a + k * 4, frand(-600, 600));
    for (u32 k = 0; k < 4; ++k) sf(b + k * 4, n % 50 == 0 ? lf(a + k * 4) : frand(-600, 600));
    for (u32 k = 0; k < 4; ++k) sf(c0 + k * 4, frand(0, 1));
    for (u32 k = 0; k < 16; ++k) sf(0x821A0000 + 7624 + k * 4, frand(0.5f, 3));  // (its scale at +32)
    const double w = (double)frand(0.1f, 8) + (n & 1 ? 1e-9 : 0);
    const bool ok = twice(kLineQuad, 0x2000,
        [&](int pass, PPCContext& ctx) {
          ctx.r3.u64 = a;
          ctx.r4.u64 = b;
          ctx.r5.u64 = c0;
          ctx.f1.f64 = w;
          pass == 0 ? original_sub_8205EF18(ctx, gw_host(0)) : sub_8205EF18(ctx, gw_host(0));
        },
        [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, 0, 0, 0}}; });
    if (!ok) ++differ;
  }
  g_alloc_stream = 0;
  printf("second line style (0x8205EF18): %d of 20000 differ%s\n", differ, speed());
  return differ ? 1 : 0;
}

// The world update (0x820366A0) and the score display (0x8207CA48): the
// game's data (0x82180000-0x821C0000) from the image with the state words
// they branch on varied, callees answering from a fixed sequence (a
// pointer to zeroed memory, its low byte zero half the time); the data, the call log and the registers compared.
__declspec(noinline) int check_big(bool hud) {
  int differ = 0;
  constexpr u32 G = 0x82199470;  // the game's state (r22 / r31 in both)
  static u8 data[0x40000];
  memcpy(data, gw_host(0x82180000), sizeof(data));
  for (int n = 0; n < 1000; ++n) {
    memcpy(gw_host(0x82180000), data, sizeof(data));
    memset(gw_host(kAnswerArea), 0, 0x1000);
    for (u32 k = 0; k < 8; ++k) gw_store32(kAnswerArea + k * 128 + 12, rand() & 1);  // (a flag the callers read there)
    const u32 modes[] = {0, 1, 2, 3, 3, 4, 5, 5, 6, 7, 8};
    gw_store32(G + 15844, modes[rand() % 11]);
    gw_store32(G + 15348, rand() % 5);
    gw_store32(G + 15860, rand() % 3);
    gw_store32(G + 15868, rand() % 4);
    sf(G + 14840, rand() & 1 ? frand(-50, 200) : frand(0, 4000));
    gw_store8(G + 15877, rand() & 1);
    // The other words and flags they branch on (from their compares).
    const u32 byte_offsets[] = {14848, 14849, 14850, 14852, 14860, 14861, 15876, 15448, 15449};
    for (const u32 off : byte_offsets) gw_store8(G + off, rand() % 4 == 0);
    const u32 flag_bytes[] = {0x8219D2B4, 0x8219D2D8, 0x8219D2FC, 0x8219D3AC};
    for (const u32 at : flag_bytes) gw_store8(at, rand() & 1);
    gw_store32(G + 14864, rand() % 12);
    gw_store32(G + 14912, rand() % 3);
    gw_store32(G + 14976, rand() % 2 ? 75 + rand() % 3 - 1 : rand() % 80);
    gw_store32(G + 15836, rand() % 4);
    gw_store32(G + 15452, rand() % 40);
    for (u32 k = 0; k < 4; ++k) {
      gw_store64(G + 15392 + k * 120, (u64)rand() << 20 | (u64)rand());
      sf(G + 15440 + k * 120, frand(-5, 5));
      sf(G + 15444 + k * 120, frand(-5, 5));
    }
    sf(0x8219D280, frand(0.005f, 0.05f));
    // Entities in the game's lists (players, enemies, the rest), their
    // virtuals the logged fakes; their memory hashed into the log.
    random_entities(48, 500);
    // (Other cases' owners share this memory with the vtable: all its slots again.)
    gw_store32(kVtbl + 4, kFakeDraw4);
    gw_store32(kVtbl + 8, kFakeDraw8);
    gw_store32(kVtbl + 12, kFakeUpdate);
    gw_store32(kVtbl + 16, kFakeAfter);
    gw_store32(kVtbl + 20, kFakePushed);
    gw_store32(kVtbl + 24, kFakeMatrix);
    gw_store32(kVtbl + 36, kFakeExpired);
    gw_store32(kVtbl + 48, kFakeTouch);
    for (u32 k = 52; k < 64; k += 4) gw_store32(kVtbl + k, kFakeDraw8);
    gw_store32(kVtbl + 28, kFakeDraw8);
    gw_store32(kVtbl + 32, kFakeDraw8);
    gw_store32(kVtbl + 40, kFakeDraw8);
    gw_store32(kVtbl + 44, kFakeDraw8);
    gw_store32(kVtbl + 0, kFakeDraw8);
    for (u32 i = 0; i < 48; ++i) {
      const u32 e = kEntsT + i * kEntSz;
      const u32 pick[] = {0, 0, 0x2, 0x10, 0x104, 0x1, 0x8, 0x800, 0x400, 0x3};
      gw_store32(e + 36, pick[rand() % 10]);
      gw_store32(e + 168, rand() % 4);
    }
    const u32 lists[] = {5032, 5008, 5080, 5068, 5020, 5044, 5056, 5200};
    for (u32 l = 0; l < 8; ++l) {
      u32 ents[8];
      const u32 count = rand() % 6;
      for (u32 i = 0; i < count; ++i) ents[i] = kEntsT + (l * 6 + i) * kEntSz;
      make_list(0x82180000 + lists[l], kNodesT + l * 0x100, ents, count);
    }
    static u8 ents_saved[48 * kEntSz];
    memcpy(ents_saved, gw_host(kEntsT), sizeof(ents_saved));
    const bool ok = twice(0x82180000, 0x40000,
        [&](int pass, PPCContext& ctx) {
          g_answer_n = 0;
          g_random_calls = 0;
          ctx.r3.u64 = 0x12340000 + n;
          ctx.f1.f64 = 0.5;
          memcpy(gw_host(kEntsT), ents_saved, sizeof(ents_saved));
          g_log_volatiles = true;
          if (hud) pass == 0 ? original_sub_8207CA48(ctx, gw_host(0)) : sub_8207CA48(ctx, gw_host(0));
          else pass == 0 ? original_sub_820366A0(ctx, gw_host(0)) : sub_820366A0(ctx, gw_host(0));
          u32 h = 2166136261u;
          for (u32 k = 0; k < sizeof(ents_saved); ++k) h = (h ^ gw_host(kEntsT)[k]) * 16777619u;
          enemy_log(h);
          enemy_log(volatile_hash(ctx));  // every volatile register as it is left
          g_log_volatiles = false;
        },
        [&](PPCContext& ctx) { return Regs{{ctx.r1.u64, ctx.r3.u64, ctx.f1.u64, 0}}; });
    if (!ok) ++differ;
  }
  memcpy(gw_host(0x82180000), data, sizeof(data));
  printf("%s: %d of 1000 differ%s\n", hud ? "score display (0x8207CA48)" : "world update (0x820366A0)", differ, speed());
  return differ ? 1 : 0;
}

// All of src/enemy_native.cpp's checks: the shared setup, each case, the
// image restored.
__declspec(noinline) int check_enemy() {
  if (!enemy_setup()) return 1;
  int bad = 0;
  bad += check_enemy_1();
  bad += check_enemy_2();
  bad += check_enemy_3();
  bad += check_enemy_4();
  bad += check_enemy_5();
  bad += check_enemy_6();
  bad += check_enemy_7();
  bad += check_enemy_8();
  bad += check_enemy_9();
  bad += check_enemy_10();
  bad += check_enemy_11();
  bad += check_enemy_12();
  bad += check_atan2();
  bad += check_colour();
  bad += check_scene_draw();
  bad += check_line2();
  bad += check_blinker();
  bad += check_models();
  bad += check_entity_matrix();
  bad += check_burst();
  bad += check_camera();
  bad += check_line_style2();
  bad += check_big(false);
  bad += check_big(true);
  enemy_teardown();
  return bad;
}

int main(int argc, char** argv) {
  SetUnhandledExceptionFilter(report_fault);
  u32 seed = argc > 1 ? (u32)atoi(argv[1]) : 1;
  if (!map(0x82000000, 0x220000) || !map(kStack - 0x10000, 0x20000) || !map(kArgs, 0x20000) ||
      !map(kGrid - kRow, 91 * kRow) || !map(kLines, 88 * 1052 * 28 + 0x1000) || !map(kDisplace, 89 * 133 * 28 + 0x1000) || !map(kStarBuffer, 3 * 0x30000) ||
      !map(kPart, 0x40000) || !map(kVerts, 0x600000 + 0x1000)) {
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
        // The native pass: the glow quad goes to a host mirror (host-order
        // floats), compared big-endian like the original's.
        memset(g_test_mirror_host, 0, sizeof(g_test_mirror_host));
        g_test_mirror_guest = kGlowQuad;
        g_test_mirror_size = pass == 0 ? 0 : 112;
        if (pass == 0) original_sub_82063448(ctx, gw_host(0));
        else sub_82063448(ctx, gw_host(0));
        g_test_mirror_size = 0;
        u8(*dst)[128] = pass == 0 ? want : got;
        memcpy(dst[0], gw_host(kOut), 44);
        memcpy(dst[1], gw_host(kStreak), 128);
        if (pass == 0) {
          memcpy(dst[2], gw_host(kGlowQuad), 112);
        } else {
          for (u32 o = 0; o < 112; o += 4) {
            u32 v;
            memcpy(&v, g_test_mirror_host + o, 4);
            v = __builtin_bswap32(v);
            memcpy(dst[2] + o, &v, 4);
          }
        }
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


  // The jobs' particle shares (guest 0x82063A38, six jobs in turn over one
  // particle system) and the bucket index sort after them (0x82064110).
  {
    constexpr u32 kSys = kPart, kBuckets = kPart + 0x1000, kIndex = kPart + 0x30000, kCtx = kPart + 0x31000;
    constexpr u32 kTables = kPart + 0x32000;
    constexpr u32 kBucketBytes = 5636;
    int differ = 0;
    for (int round = 0; round < 6; ++round) {
      const u32 n = 4 + rand() % 26;  // buckets
      // Some wells and holes (off in the first round); the particle constants
      // are the last test's. Both passes run the native particle update (the
      // original job calls sub_82063448, the native one particle at a time),
      // so this compares the native job's batches of four with it, bit for bit.
      gw_store32(kSys + 40 + 448, round == 0 ? 0 : (u32)(rand() % 4));
      gw_store32(kSys + 40 + 452, round == 0 ? 0 : (u32)(rand() % 3));
      for (u32 w = 0; w < 4; ++w) {
        for (u32 c = 0; c < 3; ++c) sf(kSys + 40 + w * 12 + c * 4, frand(-500, 500));
        for (u32 c = 0; c < 3; ++c) sf(kSys + 40 + 192 + w * 12 + c * 4, frand(-500, 500));
        sf(kSys + 40 + 384 + w * 4, frand(-3, 3));
      }
      gw_store32(kSys + 0, n);
      gw_store32(kSys + 4, kBuckets);
      gw_store32(kSys + 8, kIndex);
      gw_store32(kSys + 12, n - 1 - (u32)(rand() % 3));
      gw_store32(kSys + 496, 0);
      for (u32 b = 0; b < n; ++b) gw_store32(kIndex + b * 4, b);
      for (u32 b = n - 1; b > 0; --b) {  // shuffled
        const u32 o = (u32)rand() % (b + 1), t = gw_load32(kIndex + b * 4);
        gw_store32(kIndex + b * 4, gw_load32(kIndex + o * 4));
        gw_store32(kIndex + o * 4, t);
      }
      for (u32 b = 0; b < n; ++b) {
        const u32 bucket = kBuckets + b * kBucketBytes, count = (u32)rand() % 129;
        gw_store32(bucket + 5632, count);
        for (u32 k = 0; k < 128; ++k) {
          const u32 q = bucket + k * 44;
          for (u32 c = 0; c < 3; ++c) sf(q + c * 4, frand(-700, 700) * (c == 2 ? 0.01f : 1.0f));
          for (u32 c = 0; c < 3; ++c) sf(q + 12 + c * 4, frand(-3000, 3000) * (c == 2 ? 0.01f : 1.0f));
          for (u32 c = 0; c < 3; ++c) sf(q + 24 + c * 4, frand(0, 1));
          sf(q + 36, rand() % 3 == 0 ? frand(-1, -0.01f) : frand(0, 1));
          sf(q + 40, frand(0, 60));
        }
      }
      // Vertex buffers: frame 1, six jobs; the lock stand-in adds kLockedOffset.
      gw_store32(0x821B3E84, 1);
      gw_store32(0x821B3E80, kTables);
      gw_store32(0x821B3E7C, kTables + 0x100);
      for (u32 s = 0; s < 12; ++s) {
        gw_store32(kTables + s * 4, kVerts + s * 0x40000);
        gw_store32(kTables + 0x100 + s * 4, kVerts + 0x300000 + s * 0x40000);
      }
      for (u32 job = 0; job < 6; ++job) gw_store32(kCtx + job * 16 + 4, job);
      const u32 sizes[4] = {0x32000, 0x100, 0x600000, 16};
      const u32 starts[4] = {kPart, 0x821B3E88 - 0x80, kVerts, 0};
      u8* before_part = (u8*)malloc(0x32000);
      u8* before_verts = (u8*)malloc(0x600000);
      memcpy(before_part, gw_host(kPart), 0x32000);
      memcpy(before_verts, gw_host(kVerts), 0x600000);
      u8* want_part = (u8*)malloc(0x32000);
      u8* want_verts = (u8*)malloc(0x600000);
      u8 want_live[24];
      // Host mirrors for job 0's streak and glow buffers (frame 1: table
      // entry 6, locked at +kLockedOffset), 40 particles: the first
      // particles' quads go there, the rest big-endian to the buffers.
      static u8 want_mirror[2][8192];
      for (int pass = 0; pass < 2; ++pass) {
        memcpy(gw_host(kPart), before_part, 0x32000);
        memcpy(gw_host(kVerts), before_verts, 0x600000);
        memset(g_test_mirror_host, 0, sizeof(g_test_mirror_host));
        memset(g_test_mirror2_host, 0, sizeof(g_test_mirror2_host));
        g_test_mirror_guest = kVerts + 6 * 0x40000 + kLockedOffset;
        g_test_mirror_size = 40 * 128;
        g_test_mirror2_guest = kVerts + 0x300000 + 6 * 0x40000 + kLockedOffset;
        g_test_mirror2_size = 40 * 112;
        // The native job keeps the particles in host order (the store in
        // src/particle_native.cpp): turned to it before, and back after.
        if (pass == 1) gw_particle_store_swap(kBuckets, n);
        for (u32 job = 0; job < 6; ++job) {
          static PPCContext ctx;
          ctx = PPCContext{};
          ctx.r1.u64 = kStack;
          ctx.r3.u64 = kSys;
          ctx.r4.u64 = kCtx + job * 16;
          if (pass == 0) original_sub_82063A38(ctx, gw_host(0));
          else sub_82063A38(ctx, gw_host(0));
        }
        if (pass == 1) gw_particle_store_swap(kBuckets, n);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kSys + 8;
        ctx.r4.u64 = gw_load32(kSys);
        ctx.r5.u64 = 0;
        if (pass == 0) original_sub_82064110(ctx, gw_host(0));
        else sub_82064110(ctx, gw_host(0));
        if (pass == 0) {
          memcpy(want_part, gw_host(kPart), 0x32000);
          memcpy(want_verts, gw_host(kVerts), 0x600000);
          memcpy(want_live, gw_host(0x821B3E88), 24);
          memcpy(want_mirror[0], g_test_mirror_host, sizeof(want_mirror[0]));
          memcpy(want_mirror[1], g_test_mirror2_host, sizeof(want_mirror[1]));
        } else {
          for (u32 o = 0; o < 0x32000; o += 4) differ += memcmp(want_part + o, gw_host(kPart + o), 4) != 0;
          for (u32 o = 0; o < 0x600000; o += 4) differ += memcmp(want_verts + o, gw_host(kVerts + o), 4) != 0;
          differ += memcmp(want_live, gw_host(0x821B3E88), 24) != 0;
          for (u32 o = 0; o < sizeof(want_mirror[0]); o += 4) {
            differ += memcmp(want_mirror[0] + o, g_test_mirror_host + o, 4) != 0;
            differ += memcmp(want_mirror[1] + o, g_test_mirror2_host + o, 4) != 0;
          }
        }
        g_test_mirror_size = g_test_mirror2_size = 0;
      }
      (void)sizes;
      (void)starts;
      free(before_part);
      free(before_verts);
      free(want_part);
      free(want_verts);
    }
    printf("particle shares and bucket sort: %d words differ\n", differ);
    bad += differ;
  }

  bad += check_particle_store();



  // Scissor (guest 0x8209D288): random viewports (some huge, negative or NaN)
  // and rectangles, the clamp on and off.
  {
    constexpr u32 kDevice = kPart, kRect = kPart + 0x8000;
    int differ = 0;
    for (int n = 0; n < 20000; ++n) {
      auto any_float = [&]() {
        const int k = rand() % 10;
        if (k == 0) return NAN;
        if (k == 1) return frand(-5e9f, 5e9f);
        return frand(-300, 1600);
      };
      for (u32 o = 0; o < 4; ++o) sf(kDevice + 12648 + o * 4, any_float());
      for (u32 o = 0; o < 4; ++o) gw_store32(kRect + o * 4, (u32)(rand() % 2000 - 300));
      gw_store32(kDevice + 11856, rand() % 2);
      gw_store32(kDevice + 10436, (u32)rand() * 65537u);
      gw_store32(kDevice + 10440, (u32)rand() * 65537u);
      u8 before[16384];
      memcpy(before, gw_host(kDevice + 10000), sizeof(before));
      u8 want[16384];
      u64 want_args[5];
      for (int pass = 0; pass < 2; ++pass) {
        memcpy(gw_host(kDevice + 10000), before, sizeof(before));
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kDevice;
        ctx.r4.u64 = kRect;
        if (pass == 0) original_sub_8209D288(ctx, gw_host(0));
        else sub_8209D288(ctx, gw_host(0));
        if (pass == 0) {
          memcpy(want, gw_host(kDevice + 10000), sizeof(want));
          memcpy(want_args, g_scissor_args, sizeof(want_args));
        } else {
          differ += memcmp(want, gw_host(kDevice + 10000), sizeof(want)) != 0;
          differ += memcmp(want_args, g_scissor_args, sizeof(want_args)) != 0;
        }
      }
    }
    printf("scissor: %d of 20000 differ\n", differ);
    bad += differ;
  }


  // Set vertex / pixel shader (guest 0x820A68B0, 0x820A65F8): random devices,
  // outgoing shaders and constant blocks.
  {
    constexpr u32 kDevice = kPart, kShaders = kPart + 0x10000;
    int differ = 0;
    for (int n = 0; n < 6000; ++n) {
      const bool vertex = n & 1;
      for (u32 o = 0; o < 0x4000; o += 4) gw_store32(kDevice + o, (u32)rand() * 2654435761u);
      for (u32 o = 0; o < 0x3000; o += 4) gw_store32(kShaders + o, (u32)rand() * 2246822519u);
      gw_store32(kDevice + 10908, rand() % 4 == 0 ? (u32)rand() : 0);
      gw_store32(kDevice + 12696, rand() % 4 ? kShaders + 0x2000 : 0);
      gw_store32(kDevice + 12692, rand() % 4 ? kShaders + 0x2400 : 0);
      gw_store32(kDevice + 13524, kPart + 0x9000);
      gw_store32(kDevice + 13528, rand() % 3 ? kPart + 0x9100 : kPart + 0x8000);
      const u32 shader = rand() % 8 ? kShaders : 0;
      const u32 block = kShaders + (vertex ? 872 : 40);
      // Offset 0 (no block): the records go elsewhere, block + 20 stays 0.
      const u32 offset = rand() % 6 ? 64 : 0;
      gw_store32(block + 20, offset);
      const u32 q = offset ? block + offset : kShaders + 0x2800;
      gw_store64(q + 0, ((u64)rand() << 40) ^ ((u64)rand() << 20) ^ (u64)rand());
      gw_store64(q + 8, rand() % 2 ? 0 : (u64)rand());
      u32 at = q + 20;
      for (int k = rand() % 3; k > 0; --k, at += 8) gw_store32(at, (u32)(rand() % 100) << 16 | (1 + rand() % 5));
      gw_store32(at, (u32)(rand() % 100) << 16 | 0);
      at += 4;
      for (int k = rand() % 3; k > 0; --k) {
        const u32 count = 1 + rand() % 6;
        gw_store32(at, (u32)(rand() % 64) * 4 << 16 | count);
        at += 4 + count * 4;
      }
      gw_store32(at, 0);
      at += 4;
      for (int k = rand() % 3; k > 0; --k) {
        const u32 count = 2 * (1 + rand() % 4);
        gw_store32(at, (u32)(rand() % 64) * 4 << 16 | count);
        at += 4 + count / 2 * 8;
      }
      gw_store32(at, 0);
      at += 4;
      gw_store32(q + 16, rand() % 5 ? at - (q + 20) : (u32)(rand() % 40));
      u8* before = (u8*)malloc(0x14000);
      u8* want = (u8*)malloc(0x14000);
      memcpy(before, gw_host(kPart), 0x14000);
      u8 stack_before[256], stack_want[256];
      memcpy(stack_before, gw_host(kStack - 256), 256);
      for (int pass = 0; pass < 2; ++pass) {
        memcpy(gw_host(kPart), before, 0x14000);
        memcpy(gw_host(kStack - 256), stack_before, 256);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kDevice;
        ctx.r4.u64 = shader;
        if (pass == 0) (vertex ? original_sub_820A68B0 : original_sub_820A65F8)(ctx, gw_host(0));
        else (vertex ? sub_820A68B0 : sub_820A65F8)(ctx, gw_host(0));
        if (pass == 0) {
          memcpy(want, gw_host(kPart), 0x14000);
          memcpy(stack_want, gw_host(kStack - 256), 256);
        } else {
          differ += memcmp(want, gw_host(kPart), 0x14000) != 0;
          differ += memcmp(stack_want + 208, gw_host(kStack - 48), 8) != 0;  // the packet slot only (the original also saves registers there)
        }
      }
      free(before);
      free(want);
    }
    printf("shader sets: %d of 6000 differ\n", differ);
    bad += differ;
  }


  // Object outline (guest 0x82061DA0): random models, parts and matrices;
  // both run the native vector routine and line builder, so this checks the
  // loop, the colours and the arguments it passes.
  {
    constexpr u32 kModel = 0xC0785000, kMesh = 0xC0785100, kParts = 0xC0785200, kSegments = 0xC0785400;
    constexpr u32 kMatrix = 0xC0786000, kTint = 0xC0786100;
    auto put = [](u32 at, float v) {
      u32 bits;
      memcpy(&bits, &v, 4);
      gw_store32(at, bits);
    };
    int differ = 0;
    for (int n = 0; n < 2000; ++n) {
      const u32 parts = 1 + rand() % 3;
      gw_store32(kModel + 44, rand() % 10 ? kMesh : 0);
      gw_store32(kMesh + 0, kParts);
      gw_store32(kMesh + 4, parts);
      put(kMesh + 12, random_float(0, 8));
      for (u32 q = 0; q < parts; ++q) {
        gw_store32(kParts + q * 12 + 0, kSegments + q * 8 * 48);
        gw_store32(kParts + q * 12 + 4, (u32)(rand() % 9));
      }
      for (u32 o = 0; o < 3 * 8 * 48; o += 4) put(kSegments + o, random_float(-60, 60));
      for (u32 o = 0; o < 64; o += 4) put(kMatrix + o, random_float(-2, 2));
      for (u32 o = 0; o < 16; o += 4) put(kTint + o, random_float(0, 1.5f));
      const u32 part = (u32)(rand() % (parts + 1));
      const u32 model = rand() % 20 ? kModel : 0;
      static u8 want[0x2000 + 48];
      for (int pass = 0; pass < 2; ++pass) {
        memset(gw_host(kLineQuad), 0, 0x1000);
        memset(gw_host(kLineOutline), 0, 0x2000);
        gw_store32(0x821A0000 + 12424 + 2052, kLineQuad);
        gw_store32(0x821A0000 + 12424 + 2056, 0);
        gw_store32(0x821A0000 + 14536 + 2052, kLineOutline);
        gw_store32(0x821A0000 + 14536 + 2056, 0);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kMatrix;
        ctx.r4.u64 = model;
        ctx.r5.u64 = kTint;
        ctx.r6.u64 = part;
        if (pass == 0) original_sub_82061DA0(ctx, gw_host(0));
        else sub_82061DA0(ctx, gw_host(0));
        static u8 got[0x2000 + 48 + 0x1000];
        memcpy(got, gw_host(kLineQuad), 0x1000);
        memcpy(got + 0x1000, gw_host(kLineOutline), 0x2000);
        memcpy(got + 0x3000, gw_host(kStack - 208 + 80), 48);
        static u8 want_all[sizeof(got)];
        if (pass == 0) memcpy(want_all, got, sizeof(got));
        else differ += memcmp(want_all, got, sizeof(got)) != 0;
      }
      (void)want;
    }
    printf("object outline: %d of 2000 differ\n", differ);
    bad += differ;
  }


  // SetViewport (guest 0x8209D8A0): random viewports, targets and tiling
  // states; both call the native scissor (recorded by its callee).
  {
    constexpr u32 kDevice = kPart, kSurface = kPart + 0x8000;
    int differ = 0;
    for (int n = 0; n < 20000; ++n) {
      for (u32 o = 0; o < 0x3800; o += 4) gw_store32(kDevice + 10000 + o, (u32)rand() * 2654435761u);
      gw_store32(kDevice + 12440, rand() % 8 ? kSurface : 0);
      gw_store32(kDevice + 12456, rand() % 2 ? kSurface : 0);
      gw_store8(kDevice + 10940, (u8)(rand() & 0x30));
      for (u32 k = 0; k < 5; ++k)
        gw_store32(kDevice + 12728 + k * 4, rand() % 3 ? gw_load32(kDevice + 12440 + k * 4) : (u32)rand());
      gw_store32(kDevice + 13180, 1 + rand() % 1500);
      gw_store32(kDevice + 13184, 1 + rand() % 900);
      gw_store32(kSurface + 36, (u32)(rand() % 1500) << 18 | (u32)(rand() % 900) << 3);
      const double v[6] = {random_float(-50, 1300), random_float(-50, 800), random_float(-10, 1400),
                           random_float(-10, 900), random_float(0, 1), random_float(0, 1)};
      static u8 before[0x3800], want[0x3800];
      memcpy(before, gw_host(kDevice + 10000), sizeof(before));
      u64 want_args[5];
      for (int pass = 0; pass < 2; ++pass) {
        memcpy(gw_host(kDevice + 10000), before, sizeof(before));
        memset(g_scissor_args, 0, sizeof(g_scissor_args));
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kDevice;
        ctx.f1.f64 = v[0];
        ctx.f2.f64 = v[1];
        ctx.f3.f64 = v[2];
        ctx.f4.f64 = v[3];
        ctx.f5.f64 = v[4];
        ctx.f6.f64 = v[5];
        if (pass == 0) original_sub_8209D8A0(ctx, gw_host(0));
        else sub_8209D8A0(ctx, gw_host(0));
        if (pass == 0) {
          memcpy(want, gw_host(kDevice + 10000), sizeof(want));
          memcpy(want_args, g_scissor_args, sizeof(want_args));
        } else {
          differ += memcmp(want, gw_host(kDevice + 10000), sizeof(want)) != 0;
          differ += memcmp(want_args, g_scissor_args, 4 * sizeof(u64)) != 0;  // not r1
        }
      }
    }
    printf("set viewport: %d of 20000 differ\n", differ);
    bad += differ;
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

  // Glowing-line builder (guest 0x8205FBA8): random segments and colours,
  // the vertex allocator stubbed (fixed buffers, see sub_82061750 above).
  {
    int differ = 0;
    float worst = 0;
    for (int n = 0; n < 2000; ++n) {
      const u32 colour = 0xC0782000;  // inside the starfield buffers' mapping
      for (int c = 0; c < 4; ++c) {
        const float v = random_float(0, 2);
        u32 bits;
        memcpy(&bits, &v, 4);
        gw_store32(colour + c * 4, bits);
      }
      const double x0 = random_float(-300, 300), y0 = random_float(-200, 200);
      const double x1 = x0 + random_float(-20, 20), y1 = y0 + random_float(-20, 20);
      static u8 want[384];
      for (int pass = 0; pass < 2; ++pass) {
        memset(gw_host(kLineQuad), 0, 128);
        memset(gw_host(kLineOutline), 0, 256);
        // The two streams' chunks: the stubbed allocator's buffers, empty
        // (the native code takes room itself while a chunk has some).
        gw_store32(0x821A0000 + 16648 + 2052, kLineQuad);
        gw_store32(0x821A0000 + 16648 + 2056, 0);
        gw_store32(0x821A0000 + 18760 + 2052, kLineOutline);
        gw_store32(0x821A0000 + 18760 + 2056, 0);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.f1.f64 = x0;
        ctx.f2.f64 = y0;
        ctx.f3.f64 = x1;
        ctx.f4.f64 = y1;
        ctx.r7.u64 = colour;
        if (pass == 0) original_sub_8205FBA8(ctx, gw_host(0));
        else sub_8205FBA8(ctx, gw_host(0));
        u8 got[384];
        memcpy(got, gw_host(kLineQuad), 128);
        memcpy(got + 128, gw_host(kLineOutline), 256);
        if (pass == 0) {
          memcpy(want, got, sizeof(got));
          continue;
        }
        bool bad = false;
        for (int w = 0; w < 96; ++w) {
          u32 a, b;
          memcpy(&a, want + w * 4, 4);
          memcpy(&b, got + w * 4, 4);
          a = __builtin_bswap32(a);
          b = __builtin_bswap32(b);
          float fa, fb;
          memcpy(&fa, &a, 4);
          memcpy(&fb, &b, 4);
          const float err = fabsf(fa - fb) / (1.0f + fabsf(fa));
          if (!(err <= worst)) worst = err;
          if (!(err <= 1e-5f)) bad = true;
        }
        differ += bad;
      }
    }
    printf("glowing-line builder: %d of 2000 differ (worst %.2g)\n", differ, worst);
    if (differ) ++bad;
  }

  // 3D glowing-line builder (guest 0x8205EB58).
  {
    int differ = 0;
    float worst = 0;
    constexpr u32 kFrom = 0xC0784000, kTo = 0xC0784010, kColour = 0xC0784020;
    auto put = [](u32 at, float v) {
      u32 bits;
      memcpy(&bits, &v, 4);
      gw_store32(at, bits);
    };
    for (int n = 0; n < 2000; ++n) {
      for (int c = 0; c < 4; ++c) put(kColour + c * 4, random_float(0, 2));
      for (int c = 0; c < 3; ++c) {
        const float a = random_float(-300, 300);
        put(kFrom + c * 4, a);
        put(kTo + c * 4, a + random_float(-30, 30));
      }
      const double cap = random_float(0, 10);
      static u8 want[384];
      for (int pass = 0; pass < 2; ++pass) {
        memset(gw_host(kLineQuad), 0, 128);
        memset(gw_host(kLineOutline), 0, 256);
        gw_store32(0x821A0000 + 12424 + 2052, kLineQuad);
        gw_store32(0x821A0000 + 12424 + 2056, 0);
        gw_store32(0x821A0000 + 14536 + 2052, kLineOutline);
        gw_store32(0x821A0000 + 14536 + 2056, 0);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kFrom;
        ctx.r4.u64 = kTo;
        ctx.r5.u64 = kColour;
        ctx.f1.f64 = cap;
        if (pass == 0) original_sub_8205EB58(ctx, gw_host(0));
        else sub_8205EB58(ctx, gw_host(0));
        u8 got[384];
        memcpy(got, gw_host(kLineQuad), 128);
        memcpy(got + 128, gw_host(kLineOutline), 256);
        if (pass == 0) {
          memcpy(want, got, sizeof(got));
          continue;
        }
        bool bad = false;
        for (int w = 0; w < 96; ++w) {
          u32 a, b;
          memcpy(&a, want + w * 4, 4);
          memcpy(&b, got + w * 4, 4);
          a = __builtin_bswap32(a);
          b = __builtin_bswap32(b);
          float fa, fb;
          memcpy(&fa, &a, 4);
          memcpy(&fb, &b, 4);
          const float err = fabsf(fa - fb) / (1.0f + fabsf(fa));
          if (!(err <= worst)) worst = err;
          if (!(err <= 1e-5f)) bad = true;
        }
        differ += bad;
      }
    }
    printf("3D glowing-line builder: %d of 2000 differ (worst %.2g)\n", differ, worst);
    if (differ) ++bad;
  }

  // Vector times matrix (guest 0x82030338) and matrix times matrix (guest
  // 0x82041CA0; the original calls the vector routine, here the native one,
  // so this checks its rows and temporaries), out also aliasing an input.
  {
    constexpr u32 kA = 0xC0783000, kB = 0xC0783100, kOut = 0xC0783200;
    int differ = 0;
    float worst = 0;
    auto fill = [](u32 at, int n) {
      for (int i = 0; i < n; ++i) {
        const float v = random_float(-100, 100);
        u32 bits;
        memcpy(&bits, &v, 4);
        gw_store32(at + i * 4, bits);
      }
    };
    auto compare_floats = [&](const u8* want, const u8* got, int n) {
      bool bad = false;
      for (int i = 0; i < n; ++i) {
        u32 a, b;
        memcpy(&a, want + i * 4, 4);
        memcpy(&b, got + i * 4, 4);
        a = __builtin_bswap32(a);
        b = __builtin_bswap32(b);
        float fa, fb;
        memcpy(&fa, &a, 4);
        memcpy(&fb, &b, 4);
        const float err = fabsf(fa - fb) / (1.0f + fabsf(fa));
        if (!(err <= worst)) worst = err;
        if (!(err <= 1e-6f)) bad = true;
      }
      return bad;
    };
    for (int n = 0; n < 2000; ++n) {
      fill(kA, 16);
      fill(kB, 16);
      const int which = n % 3;  // 0: vector, 1: matrix, 2: matrix into a
      u8 want[64], saved[128];
      memcpy(saved, gw_host(kA), 64);
      memcpy(saved + 64, gw_host(kB), 64);
      for (int pass = 0; pass < 2; ++pass) {
        memcpy(gw_host(kA), saved, 64);
        memcpy(gw_host(kB), saved + 64, 64);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = which == 2 ? kA : kOut;
        ctx.r4.u64 = kA;
        ctx.r5.u64 = kB;
        if (which == 0) {
          if (pass == 0) original_sub_82030338(ctx, gw_host(0));
          else sub_82030338(ctx, gw_host(0));
        } else {
          if (pass == 0) original_sub_82041CA0(ctx, gw_host(0));
          else sub_82041CA0(ctx, gw_host(0));
        }
        const u8* got = gw_host(which == 2 ? kA : kOut);
        if (pass == 0) memcpy(want, got, 64);
        else differ += compare_floats(want, got, which == 0 ? 4 : 16);
      }
    }
    printf("vector/matrix products: %d of 2000 differ (worst %.2g)\n", differ, worst);
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

  // Cosine (guest 0x8212EA80): as the sine below.
  {
    static PPCContext ctx;
    double worst = 0, worst_x = 0;
    const double edges[] = {0.0, -0.0, 1e-30, -1e-30, 1.5707963, 3.1415926, -3.1415926, 2047.9, 4096.0, 1e9};
    for (int n = 0; n < 200000 + 10; ++n) {
      double x = n < 10 ? edges[n] : (double)frand(-2100, 2100) * (n & 1 ? 1.0 : 1e-3);
      x = (double)(float)x;
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      ctx.f1.f64 = x;
      original_sub_8212EA80(ctx, gw_host(0));
      const double want = ctx.f1.f64;
      ctx = PPCContext{};
      ctx.r1.u64 = kStack;
      ctx.f1.f64 = x;
      sub_8212EA80(ctx, gw_host(0));
      const double got = ctx.f1.f64;
      const double err = want != want ? (got != got ? 0 : 1) : fabs(want - got);
      if (err > worst) {
        worst = err;
        worst_x = x;
      }
    }
    printf("cosine: worst difference %.2g (at x = %g)\n", worst, worst_x);
    if (worst > 1e-5) ++bad;
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

  // Rotation about an axis (guest 0x82041600): random unit-ish axes and
  // angles; the 16 floats compared exactly (both use the native sine and
  // cosine, so this checks the matrix arithmetic).
  {
    constexpr u32 kOutM = kArgs + 0x3000, kAxis = kArgs + 0x3100;
    int differ = 0;
    for (int n = 0; n < 20000; ++n) {
      for (int k = 0; k < 3; ++k) sf(kAxis + k * 4, frand(-1, 1));
      const double angle = (double)frand(-7, 7);
      u8 want[64];
      for (int pass = 0; pass < 2; ++pass) {
        memset(gw_host(kOutM), 0x55, 64);
        static PPCContext ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = kStack;
        ctx.r3.u64 = kOutM;
        ctx.r4.u64 = kAxis;
        ctx.f1.f64 = angle;
        if (pass == 0) original_sub_82041600(ctx, gw_host(0));
        else sub_82041600(ctx, gw_host(0));
        if (pass == 0) memcpy(want, gw_host(kOutM), 64);
        else if (memcmp(want, gw_host(kOutM), 64) != 0) ++differ;
      }
    }
    printf("rotation matrix: %d of 20000 differ\n", differ);
    if (differ) ++bad;
  }

  // Separation (guest 0x82046248, src/entity_native.cpp): random enemies of
  // three kinds in the game's 32 x 32 spatial hash (some cells crowded,
  // some on top of each other, some near the wrap), each pushed in turn by
  // both versions; every entity's 72 bytes compared. The random push
  // (sub_8203B8B0) is a stand-in giving the same vectors in the same order.
  {
    constexpr u32 kArea = 0x70300000, kTable = kArea, kEntries = kArea + 0x4000, kEnts = kArea + 0x8000;
    constexpr u32 kEntSize = 80, kCount = 300;
    if (!map(kArea, 0x20000)) return 1;
    const u32 saved_table = gw_load32(0x8219E850);
    gw_store32(0x8219E850, kTable);
    int differ = 0, calls = 0;
    for (int n = 0; n < 40; ++n) {
      // Empty cells point at themselves.
      for (u32 c = 0; c < 1024; ++c) {
        gw_store32(kTable + c * 16 + 4, kTable + c * 16);
        gw_store32(kTable + c * 16 + 8, kTable + c * 16);
      }
      for (u32 i = 0; i < kCount; ++i) {
        const u32 e = kEnts + i * kEntSize;
        memset(gw_host(e), 0, kEntSize);
        float x, y;
        if (i % 7 == 0 && i) {  // on top of the one before
          x = ldf_host(e - kEntSize + 4);
          y = ldf_host(e - kEntSize + 8);
        } else if (i % 5 == 0) {  // a crowd
          x = frand(90, 110);
          y = frand(-110, -90);
        } else {
          x = frand(-520, 520);
          y = frand(-520, 520);
        }
        sf(e + 4, x);
        sf(e + 8, y);
        sf(e + 12, frand(-5, 5));
        sf(e + 16, frand(-5, 5));
        sf(e + 32, frand(4, 20));
        gw_store32(e + 68, i % 3);
        // Into its cell's list, at the front.
        const u32 c = ((((u32)(i32)y >> 5) & 31) << 5 | (((u32)(i32)x >> 5) & 31));
        const u32 cellp = kTable + c * 16, entry = kEntries + i * 8;
        gw_store32(entry, e);
        gw_store32(entry + 4, gw_load32(cellp + 4));
        gw_store32(cellp + 4, entry);
      }
      static u8 start[kCount * kEntSize], want[kCount * kEntSize];
      memcpy(start, gw_host(kEnts), sizeof(start));
      for (int pass = 0; pass < 2; ++pass) {
        memcpy(gw_host(kEnts), start, sizeof(start));
        g_random_calls = 0;
        for (u32 i = 0; i < kCount; ++i) {
          static PPCContext ctx;
          ctx = PPCContext{};
          ctx.r1.u64 = kStack;
          ctx.r3.u64 = kEnts + i * kEntSize;
          ctx.f1.f64 = (double)frand_fixed(i, 10, 60);
          ctx.f2.f64 = (double)frand_fixed(i + 1000, 0.05f, 1.0f);
          if (pass == 0) original_sub_82046248(ctx, gw_host(0));
          else sub_82046248(ctx, gw_host(0));
        }
        if (pass == 0) {
          memcpy(want, gw_host(kEnts), sizeof(want));
          calls += g_random_calls;
        } else if (memcmp(want, gw_host(kEnts), sizeof(want)) != 0) {
          ++differ;
        }
      }
    }
    gw_store32(0x8219E850, saved_table);
    printf("separation: %d of 40 crowds differ (%d random pushes taken)\n", differ, calls);
    if (differ) ++bad;
  }

  // src/enemy_native.cpp's checks (check_enemy, above main).
  bad += check_enemy();

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
