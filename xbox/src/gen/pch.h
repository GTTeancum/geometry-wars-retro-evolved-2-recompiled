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

#define REX_LOAD_U8(x) (*(volatile u8*)REX_HOST(x))
#define REX_LOAD_U16(x) __builtin_bswap16(*(volatile u16*)REX_HOST(x))
#define REX_LOAD_U32(x) __builtin_bswap32(*(volatile u32*)REX_HOST(x))
#define REX_LOAD_U64(x) __builtin_bswap64(*(volatile u64*)REX_HOST(x))

#define REX_STORE_U8(x, y) (*(volatile u8*)REX_HOST(x) = (y))
#define REX_STORE_U16(x, y) (*(volatile u16*)REX_HOST(x) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(volatile u32*)REX_HOST(x) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(volatile u64*)REX_HOST(x) = __builtin_bswap64(y))

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
// Calls. Indirect calls use a host-side table, one entry per guest
// instruction slot (1.4 MB), filled from PPCFuncMappings at boot.
//=============================================================================

#define REX_CALL_FUNC(x) x(ctx, base)

extern PPCFunc* gw_dispatch_table[];

#define REX_CALL_INDIRECT_FUNC(x)                                                       \
  do {                                                                                  \
    u32 rex_indirect_target_ = (u32)(x);                                                \
    u32 rex_indirect_slot_ = rex_indirect_target_ - REX_CODE_BASE;                      \
    PPCFunc* rex_indirect_func_ =                                                       \
        rex_indirect_slot_ < REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE                     \
            ? gw_dispatch_table[rex_indirect_slot_ >> 2]                                \
            : nullptr;                                                                  \
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
