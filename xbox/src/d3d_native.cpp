// Native replacements for parts of the game's Direct3D (the Xbox 360 XDK's,
// linked into the game).
//
// Shader constant upload (guest 0x820A91A0): writes the constants marked in
// a 64-bit dirty mask into the command buffer as SET_CONSTANT runs. Each mask
// bit (from the top) stands for 4 constants (64 bytes); for each run of set
// bits it pads the write pointer with type-2 NOPs to 16-byte alignment less a
// word, writes the run's header ((count - 1) << 16 | first register) and
// copies the constants. The recompiled copy went through vector registers
// (lvx/stvx, each a byte reversal here); natively it is one copy. When the
// buffer has no room for a run, the original's helper (guest 0x820A87B0)
// writes it in pieces, as in the original.
#include "runtime.h"

extern "C" void sub_820A87B0(PPCContext& ctx, uint8_t* base);  // writes a run in pieces, making room

namespace {

constexpr u32 kNop = 0x80000000;  // PM4 type-2 packet

inline u32 leading_zeros64(u64 v) { return v ? (u32)__builtin_clzll(v) : 64; }

}  // namespace

// r3 device, r4 dirty mask (64-bit), r5 first constant register, r6 source.
extern "C" void sub_820A91A0(PPCContext& ctx, uint8_t* base) {
  const u32 device = ctx.r3.u32;
  u64 mask = ctx.r4.u64;
  u32 reg = ctx.r5.u32;
  u32 src = ctx.r6.u32;  // the original keeps src - 4
  u32 out = gw_load32(device + 48);
  while (mask) {
    const u32 skip = leading_zeros64(mask);
    src += skip * 64;
    reg += skip * 16;
    mask <<= skip;  // skip < 64 here: mask is not zero
    const u32 run = leading_zeros64(~mask);  // consecutive set bits
    const u32 words = run * 16;
    const u32 end = gw_load32(device + 52);
    if (out + (words + 3) * 4 >= end) {
      // Not enough room: the original's helper writes the run (in pieces).
      ctx.r3.u64 = device;
      ctx.r4.u64 = out;
      ctx.r5.u64 = reg;
      ctx.r6.u64 = src - 4;
      ctx.r7.u64 = words;
      ctx.r8.u64 = 16;
      sub_820A87B0(ctx, base);
      out = ctx.r3.u32;
      src += words * 4;
    } else {
      // Pad so the constants after the header start 16-byte aligned.
      const u32 pad = (out & 15) == 4 ? 1 : (out & 15) == 0 ? 2 : (out & 15) == 12 ? 3 : 0;
      for (u32 i = 0; i < pad; ++i) {
        out += 4;
        gw_store32(out, kNop);
      }
      out += 4;
      gw_store32(out, (words - 1) << 16 | reg);
      // The vector loads and stores address 16-byte aligned blocks.
      for (u32 n = 0; n < run; ++n) {
        for (u32 block = 0; block < 64; block += 16) {
          memcpy(gw_host((out + 4 + block) & ~15u), gw_host((src + block) & ~15u), 16);
        }
        src += 64;
        out += 64;
      }
    }
    reg += words;
    mask = run < 64 ? mask << run : 0;
  }
  gw_store32(device + 48, out);
}

// Shader constant shadow writes (guest 0x820A6438 vertex, 0x820A6518 pixel):
// r3 device, r4 first register, r5 source (any alignment), r6 count of
// 16-byte constants, r7 64-bit dirty bits. The constants are copied into the
// device's shadow at register 120 (vertex) or 376 (pixel) on, 16-byte
// aligned (stvx), and the dirty bits ORed into the device's first (vertex)
// or second (pixel) mask. The original copied through lvlx/lvrx/vor/stvx.
namespace {
void set_constants(PPCContext& ctx, u32 shadow_register, u32 mask_offset) {
  const u32 device = ctx.r3.u32, count = ctx.r6.u32;
  const u32 dst = (device + ((ctx.r4.u32 + shadow_register) << 4)) & ~15u;
  memmove(gw_host(dst), gw_host(ctx.r5.u32), count * 16);
  gw_store64(device + mask_offset, gw_load64(device + mask_offset) | ctx.r7.u64);
}
}  // namespace

extern "C" void sub_820A6438(PPCContext& ctx, uint8_t* base) {
  (void)base;
  set_constants(ctx, 120, 0);
}

extern "C" void sub_820A6518(PPCContext& ctx, uint8_t* base) {
  (void)base;
  set_constants(ctx, 376, 8);
}
