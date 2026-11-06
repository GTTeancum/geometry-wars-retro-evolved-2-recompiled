// Native replacements for the game's C runtime memcpy and memset (same
// addresses as src/crt_native.cpp in the PC port). Besides the speed-up they
// keep commit-on-touch memory untouched where they can:
//   memcpy from a disc-backed range records the destination as disc-backed;
//   memset to zero skips pages that were never committed (already zero).
#include "runtime.h"

#include <windows.h>

bool gw_disc_copy(u32 dst, u32 src, u32 n);

namespace {

// A forward copy equals memmove unless the destination starts inside the
// source, where it repeats the copied bytes.
bool plain_forward_copy(u32 dest, u32 source, u32 size) {
  return dest <= source || dest - source >= size;
}

// Not wrapping and clear of the GPU register window.
bool native_range(u32 address, u32 size) {
  u64 end = (u64)address + size;
  return end <= 0x100000000ull && (end <= 0x7F000000ull || address >= 0x80000000u);
}

bool committed(u32 page) {
  MEMORY_BASIC_INFORMATION info;
  return VirtualQuery(gw_host(page), &info, sizeof(info)) && info.State == MEM_COMMIT;
}

constexpr u32 kLargeCopy = 0x10000;

}  // namespace

extern "C" void __imp__sub_8212EDC0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82130E10(PPCContext& ctx, uint8_t* base);

// memcpy: r3 destination, r4 source, r5 size; returns the destination, which
// it also leaves at r1 - 8.
extern "C" void sub_8212EDC0(PPCContext& ctx, uint8_t* base) {
  u32 dest = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
  // Physical memory is only mapped at 0xC0000000; the 360's other views are not.
  auto alias = [](u32 a) { return (a >= 0xA0000000u && a < 0xC0000000u) || a >= 0xE0000000u; };
  if (alias(dest) || alias(source)) {
    gw_log("memcpy(%08X, %08X, %X) through an unmapped physical view\n", dest, source, size);
    gw_log_backtrace(ctx);
  }
  if (!plain_forward_copy(dest, source, size) || !native_range(dest, size) || !native_range(source, size)) {
    __imp__sub_8212EDC0(ctx, base);
    return;
  }
  gw_store64(ctx.r1.u32 - 8, ctx.r3.u64);
  if (size >= kLargeCopy && gw_disc_copy(dest, source, size)) return;
  memmove(gw_host(dest), gw_host(source), size);
}

extern "C" void __imp__sub_82095188(PPCContext& ctx, uint8_t* base);

// XMemCpy, the Xbox 360's cache-tuned copy: r3 destination, r4 source, r5
// size; returns the destination. The song banks are copied with this one.
extern "C" void sub_82095188(PPCContext& ctx, uint8_t* base) {
  u32 dest = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
  if (!plain_forward_copy(dest, source, size) || !native_range(dest, size) || !native_range(source, size)) {
    __imp__sub_82095188(ctx, base);
    return;
  }
  if (size < kLargeCopy || !gw_disc_copy(dest, source, size)) memmove(gw_host(dest), gw_host(source), size);
  ctx.r3.u64 = dest;
}

// memset: r3 destination, r4 fill byte, r5 size; returns the destination.
extern "C" void sub_82130E10(PPCContext& ctx, uint8_t* base) {
  u32 dest = ctx.r3.u32, size = ctx.r5.u32;
  u8 value = (u8)ctx.r4.u32;
  if (!native_range(dest, size)) {
    __imp__sub_82130E10(ctx, base);
    return;
  }
  if (value != 0 || size < kLargeCopy) {
    memset(gw_host(dest), value, size);
    return;
  }
  // Zero fill: uncommitted pages already read as zero once committed.
  u32 end = dest + size;
  for (u32 page = dest & ~0xFFFu; page < end; page += 0x1000) {
    if (!committed(page)) continue;
    u32 lo = page > dest ? page : dest;
    u32 hi = page + 0x1000 < end ? page + 0x1000 : end;
    memset(gw_host(lo), 0, hi - lo);
  }
}
