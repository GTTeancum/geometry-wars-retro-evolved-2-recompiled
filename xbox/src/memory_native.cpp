// Native replacements for the game's memory copy routines.
//
// Guest 0x82096F30 is the 360 C library's large-block copy (reached through
// its memcpy, by a function pointer): AltiVec loads with lvsl/vperm for an
// unaligned source, dcbzl to claim each 128-byte destination line before
// storing it, dcbt/dcbf to stream. Its callers pass a 128-byte-aligned
// destination and a length that is a multiple of 128 (any other length would
// never end: the loop counts down by 128 to exactly zero). As recompiled code
// every 16-byte vector goes through the emulated vector registers; here it is
// one host copy. It was the largest single function on the game's main
// thread on the Xbox.
#include "runtime.h"

#include <string.h>

extern "C" void sub_82096F30(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const u32 dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  memmove(gw_host(dst), gw_host(src), n);
  // The registers as the original leaves them: both pointers past the copy
  // (the source one 16 further, its look-ahead), the count used up, and the
  // offsets of its unrolled loop in r6-r12.
  ctx.r3.u64 = dst + n;
  ctx.r4.u64 = src + 16 + n;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 16;
  ctx.r7.u64 = 32;
  ctx.r8.u64 = 48;
  ctx.r9.u64 = 64;
  ctx.r10.u64 = 80;
  ctx.r11.u64 = 96;
  ctx.r12.u64 = 112;
}
