// Xbox replacement for the generated geometry_wars_retro_evolved_2_pch.h.
// xbox/tools/prepare_generated.py copies the generated sources into
// xbox/build/gen and puts this file in place of the generated PCH after
// checking that the image constants below still match it.
#pragma once

#define REX_CONFIG_H_INCLUDED

#define REX_IMAGE_BASE 0x82000000u
#define REX_IMAGE_SIZE 0x220000u
#define REX_CODE_BASE 0x82030000u
#define REX_CODE_SIZE 0x14A1A4u
#define REX_THUNK_RESERVE_SIZE 0x10000u

#define REXCRT_HEAP 0

#include <rex/ppc.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/chrono/clock.h>
#include <rex/thread/mutex.h>
#include <rex/system/mmio_handler.h>

#include <atomic>
#include <csetjmp>
#include <cstdlib>

extern const rex::PPCImageInfo PPCImageConfig;
extern PPCFuncMapping PPCFuncMappings[];

//=============================================================================
// Function definitions (PE/COFF: weak aliases so hooks can replace a function)
//=============================================================================

#define DECLARE_REX_FUNC(name) \
  REX_EXTERN(name);            \
  REX_EXTERN(__imp__##name)

#define DEFINE_REX_FUNC(name)                                \
  __attribute__((alias("__imp__" #name)))                    \
  __attribute__((weak, noinline)) extern "C" REX_FUNC(name); \
  REX_EXTERN(__imp__##name)

#define REX_FUNC_PROLOGUE() ((void)0)

//=============================================================================
// Guest memory: host = base + guest, wrapping in the 32-bit address space.
// The guest windows are placed so that every one of them lands in free Xbox
// virtual memory (xbox/src/guest_memory.cpp).
//=============================================================================

#define REX_PHYS_HOST_OFFSET(addr) 0u
#define REX_HOST(x) (base + (u32)(x))

#define REX_RAW_ADDR(x) REX_HOST(x)

// Plain (not volatile) accesses, so the compiler can keep and combine guest
// values within straight-line code (about 15% on the grid code, measured
// with tools/native_check). Every label in the generated code is followed by
// GW_LABEL_BARRIER (added by xbox/tools/prepare_generated.py): all loops run
// through a label, so a loop polling guest memory reloads it every pass.
// memcpy keeps unaligned guest addresses legal (plain moves on x86).
#define GW_LABEL_BARRIER() __asm__ volatile("" ::: "memory")
#define GW_LOAD(type, x) ({ type rex_v_; __builtin_memcpy(&rex_v_, REX_HOST(x), sizeof(type)); rex_v_; })
#define GW_STORE(type, x, y) do { type rex_v_ = (type)(y); __builtin_memcpy(REX_HOST(x), &rex_v_, sizeof(type)); } while (0)

#define REX_LOAD_U8(x) GW_LOAD(u8, x)
#define REX_LOAD_U16(x) __builtin_bswap16(GW_LOAD(u16, x))
#define REX_LOAD_U32(x) __builtin_bswap32(GW_LOAD(u32, x))
#define REX_LOAD_U64(x) __builtin_bswap64(GW_LOAD(u64, x))

#define REX_STORE_U8(x, y) GW_STORE(u8, x, y)
#define REX_STORE_U16(x, y) GW_STORE(u16, x, __builtin_bswap16(y))
#define REX_STORE_U32(x, y) GW_STORE(u32, x, __builtin_bswap32(y))
#define REX_STORE_U64(x, y) GW_STORE(u64, x, __builtin_bswap64(y))

//=============================================================================
// GPU register window
//=============================================================================

#define REX_IS_MMIO_ADDR(addr) ((u32)(addr) >= 0x7F000000u && (u32)(addr) < 0x80000000u)

#define REX_MM_STORE_U32(addr, val)                                                    \
  do {                                                                                 \
    u32 _mmio_addr = (addr);                                                           \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr, (u32)(val)); \
    } else {                                                                           \
      REX_STORE_U32(_mmio_addr, (val));                                                \
    }                                                                                  \
  } while (0)

#define REX_MM_LOAD_U32(addr)                                          \
  (REX_IS_MMIO_ADDR(addr) ? ({                                         \
    u32 _v;                                                            \
    rex::runtime::MMIOHandler::global_handler()->CheckLoad(addr, &_v); \
    _v;                                                                \
  })                                                                   \
                          : REX_LOAD_U32(addr))

#define REX_MM_LOAD_U16(addr)                                          \
  (REX_IS_MMIO_ADDR(addr) ? ({                                         \
    u32 _v;                                                            \
    rex::runtime::MMIOHandler::global_handler()->CheckLoad(addr, &_v); \
    (u16)_v;                                                           \
  })                                                                   \
                          : REX_LOAD_U16(addr))

//=============================================================================
// Calls. Indirect calls look the target up in a host-side hash table
// (dispatch.h), filled from PPCFuncMappings at boot.
//=============================================================================

#define REX_CALL_FUNC(x) x(ctx, base)

#include "dispatch.h"

#define REX_CALL_INDIRECT_FUNC(x)                                                       \
  do {                                                                                  \
    u32 rex_indirect_target_ = (u32)(x);                                                \
    PPCFunc* rex_indirect_func_ = gw_dispatch_find(rex_indirect_target_);               \
    if (!rex_indirect_func_) [[unlikely]] {                                             \
      ctx.last_indirect_target = rex_indirect_target_;                                  \
      rex_indirect_func_ = rex::runtime::ResolveIndirectFunction(rex_indirect_target_); \
    }                                                                                   \
    rex_indirect_func_(ctx, base);                                                      \
  } while (0)

#define REX_SET_FLUSH_MODE(enable) \
  do { if (enable) ctx.fpscr.enableFlushMode(); else ctx.fpscr.disableFlushMode(); } while (0)

[[noreturn]] void gw_fatal(const char* fmt, ...);
#define REX_UNIMPLEMENTED(addr, opcode) \
  gw_fatal("Unimplemented PPC instruction %s at %08X", opcode, (u32)(addr))

#define REX_QUERY_TIMEBASE() rex::chrono::Clock::QueryGuestTickCount()

// The Pentium III has no fused multiply-add; std::fma would be a slow exact
// library routine. prepare_generated.py rewrites std::fma to this.
// 16 bytes copied in reverse order: a vector register (kept byte-reversed in
// host order) to or from guest memory. Replaces SIMDe's shuffle_epi8 with the
// VectorMaskL table (xbox/tools/prepare_generated.py).
static inline void gw_copy_reversed(void* dst, const void* src) {
  u64 lo, hi;
  __builtin_memcpy(&lo, src, 8);
  __builtin_memcpy(&hi, (const u8*)src + 8, 8);
  lo = __builtin_bswap64(lo);
  hi = __builtin_bswap64(hi);
  __builtin_memcpy(dst, &hi, 8);
  __builtin_memcpy((u8*)dst + 8, &lo, 8);
}

// fctiwz/fctidz: double -> integer, truncating, as SSE2's cvttsd would (the
// callers have already handled NaN and the upper limit). On the Pentium III
// a C cast switches the x87 rounding mode twice, and the 64-bit one calls a
// library routine; instead, adding 1.5 * 2^52 makes the FPU round x to an
// integer held in the low mantissa bits (the volatile store rounds to double
// precision whatever the x87 precision setting), then the result is moved
// one towards zero where rounding went the other way.
static inline i64 gw_round_magic(double x, double* rounded) {
  constexpr double kMagic = 6755399441055744.0;  // 2^52 + 2^51
  volatile double m = x + kMagic;
  const double sum = m;
  *rounded = sum - kMagic;
  u64 bits;
  __builtin_memcpy(&bits, &sum, 8);
  return (i64)(bits & 0x000FFFFFFFFFFFFFull) - (1ll << 51);
}

static inline i32 gw_trunc_i32(double x) {
  if (!(x > -2147483649.0)) return INT32_MIN;
  double rounded;
  i64 n = gw_round_magic(x, &rounded);
  if (x >= 0 ? rounded > x : rounded < x) n += x >= 0 ? -1 : 1;
  return (i32)n;
}

static inline i64 gw_trunc_i64(double x) {
  if (!(__builtin_fabs(x) < 2251799813685248.0)) return simde_mm_cvttsd_si64(simde_mm_load_sd(&x));  // |x| >= 2^51
  double rounded;
  i64 n = gw_round_magic(x, &rounded);
  if (x >= 0 ? rounded > x : rounded < x) n += x >= 0 ? -1 : 1;
  return n;
}

static inline double gw_fma(double a, double b, double c) { return a * b + c; }
static inline float gw_fma(float a, float b, float c) { return a * b + c; }

//=============================================================================
// setjmp/longjmp: the game's own pair (0x82131CD0/0x82131890), keyed by the
// guest jmp_buf address.
//=============================================================================

jmp_buf* gw_jmp_buf_for(u32 guest_buf_addr, bool create);

#define ppc_setjmp(guest_buf_addr) (setjmp(*gw_jmp_buf_for((guest_buf_addr), true)))

[[noreturn]] inline void ppc_longjmp(u32 guest_buf_addr, int val) {
  jmp_buf* buf = gw_jmp_buf_for(guest_buf_addr, false);
  if (buf) longjmp(*buf, val);
  gw_fatal("longjmp to unknown guest jmp_buf %08X", guest_buf_addr);
}

inline void ppc_trap(PPCContext& ctx, u8* base, u16 trap_type) {
  (void)ctx;
  (void)base;
  if (trap_type != 25 && trap_type != 20 && trap_type != 26) gw_log("[cpu] trap type %u\n", trap_type);
}

//=============================================================================
// Interrupt locks (mfmsr/mtmsrd)
//=============================================================================

extern std::atomic<i32> gw_global_lock_count;

#define REX_CHECK_GLOBAL_LOCK()                                        \
  ([&]() -> u64 {                                                      \
    auto lock_ = rex::thread::global_critical_region::AcquireDirect(); \
    return gw_global_lock_count.load() ? 0 : 0x8000;                   \
  }())

#define REX_ENTER_GLOBAL_LOCK()                          \
  do {                                                   \
    rex::thread::global_critical_region::mutex().lock(); \
    gw_global_lock_count.fetch_add(1);                   \
  } while (0)

#define REX_LEAVE_GLOBAL_LOCK()                            \
  do {                                                     \
    gw_global_lock_count.fetch_sub(1);                     \
    rex::thread::global_critical_region::mutex().unlock(); \
  } while (0)
