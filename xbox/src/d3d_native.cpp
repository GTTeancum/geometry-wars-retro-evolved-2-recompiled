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

// Scissor (guest 0x8209D288): r3 device, r4 the rectangle to clamp to
// (left, top, right, bottom, signed words). The viewport's x, y, width and
// height (floats at +12648..+12660) truncated to integers (fctiwz,
// saturating; NaN gives 0x80000000); the rectangle kept at +12676..+12688;
// the viewport's corners clamped into it when +11856 is set; packed into the
// scissor registers' shadows (+10436 top-left, +10440 bottom-right: y in
// bits 16-30, x in bits 0-14); then sub_8209AC70 with the corners in r4-r7.
// The recompiled float-to-integer conversions made it the costliest of the
// game's Direct3D functions on the Xbox.
extern "C" void sub_8209AC70(PPCContext& ctx, uint8_t* base);
namespace {
inline float load_float(u32 a) {
  const u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline u32 fctiwz(float f) {
  if (f != f) return 0x80000000u;
  if (f >= 2147483648.0f) return 0x7FFFFFFFu;
  if (f <= -2147483648.0f) return 0x80000000u;
  return (u32)(i32)f;
}
}  // namespace

extern "C" void sub_8209D288(PPCContext& ctx, uint8_t* base) {
  const u32 device = ctx.r3.u32, rect = ctx.r4.u32;
  const u32 x = fctiwz(load_float(device + 12648)), y = fctiwz(load_float(device + 12652));
  const u32 w = fctiwz(load_float(device + 12656)), h = fctiwz(load_float(device + 12660));
  const u32 left = gw_load32(rect), top = gw_load32(rect + 4), right = gw_load32(rect + 8),
            bottom = gw_load32(rect + 12);
  gw_store32(device + 12676, left);
  gw_store32(device + 12680, top);
  gw_store32(device + 12684, right);
  gw_store32(device + 12688, bottom);
  // 64-bit registers as the PowerPC code has them (the sums are 64-bit adds
  // of zero-extended words).
  u64 x0 = x, y0 = y, x1 = (u64)w + x, y1 = (u64)h + y;
  if (gw_load32(device + 11856) != 0) {
    if (!((i32)(u32)x0 > (i32)left)) x0 = left;
    if (!((i32)(u32)y0 > (i32)top)) y0 = top;
    if (!((i32)(u32)x1 < (i32)right)) x1 = right;
    if (!((i32)(u32)y1 < (i32)bottom)) y1 = bottom;
  }
  const u32 a = gw_load32(device + 10436), b = gw_load32(device + 10440);
  gw_store32(device + 10436, (a & ~0x7FFF7FFFu) | (((u32)y0 << 16) & 0x7FFF0000u) | ((u32)x0 & 0x7FFFu));
  gw_store32(device + 10440, (b & ~0x7FFF7FFFu) | (((u32)y1 << 16) & 0x7FFF0000u) | ((u32)x1 & 0x7FFFu));
  ctx.r4.u64 = x0;
  ctx.r5.u64 = y0;
  ctx.r6.u64 = x1;
  ctx.r7.u64 = y1;
  ctx.r1.u64 = ctx.r1.u32 - 112;  // the original's frame
  sub_8209AC70(ctx, base);
  ctx.r1.u64 = ctx.r1.u32 + 112;
}

// Set vertex shader (guest 0x820A68B0) and set pixel shader (0x820A65F8):
// r3 device, r4 shader object (0: none). Both:
//   1. the outgoing shader (device + slot): if +10908 is set it goes to the
//      shader's +8; else if its +0 shares a bit with +10912, an 8-byte
//      packet {shader >> 2 with the top bit of a stale stack word, -1} at
//      the command buffer's write pointer (+13524; past +13528 the helper
//      sub_820AA9E0 makes room first), as the original, which built the
//      packet in an uninitialised stack slot (the same stores are made to it
//      here, so later reads of it see what they did);
//   2. the dirty bits (the 64-bit word at +16) and the new shader at slot;
//   3. the shader's constant block (shader + block, its offset at +20):
//      clears its mask from the dirty word at +0 (vertex) or +8 (pixel),
//      sets bit 56 of +32 if its second mask is not zero, then applies its
//      records to the fetch constant shadows (+1152): copies (count words
//      from the record) until a record with count 0, then and/or pairs.
// The vertex one also clears bit 7 of +10942 (and sets the dirty bit before
// step 1); the pixel one sets its dirty bits after step 1.
extern "C" void sub_820AA9E0(PPCContext& ctx, uint8_t* base);  // makes room in the command buffer
namespace {
inline u64 load64(u32 a) { return gw_load64(a); }
inline void store64(u32 a, u64 v) { gw_store64(a, v); }

void set_shader(PPCContext& ctx, uint8_t* base, bool vertex) {
  const u32 device = ctx.r3.u32, shader = ctx.r4.u32;
  const u32 slot = device + (vertex ? 12696 : 12692);
  const u32 frame = ctx.r1.u32 - 128;  // the original's frame (its packet slot at +80)
  if (vertex && shader) store64(device + 16, load64(device + 16) | 0x80000);
  const u32 old = gw_load32(slot);
  if (old) {
    const u32 keep = gw_load32(device + 10908);
    if (keep) {
      gw_store32(old + 8, keep);
    } else if (gw_load32(device + 10912) & gw_load32(old + 0)) {
      u32 out = gw_load32(device + 13524);
      if (out >= gw_load32(device + 13528)) {
        ctx.r1.u64 = frame;
        ctx.r3.u64 = device;
        sub_820AA9E0(ctx, base);
        ctx.r1.u64 = frame + 128;
        out = ctx.r3.u32;
      }
      const u32 word = (gw_load32(frame + 80) & 0x80000000u) | ((old >> 2) & 0x3FFFFFFFu);
      gw_store32(frame + 84, 0xFFFFFFFFu);
      gw_store32(frame + 80, word);
      gw_store32(out + 0, word);
      gw_store32(out + 4, 0xFFFFFFFFu);
      gw_store32(device + 13524, out + 8);
    }
  }
  if (vertex) {
    gw_store32(slot, shader);
    gw_store8(device + 10942, gw_load8(device + 10942) & 0x7F);
  } else {
    gw_store32(slot, shader);
    store64(device + 16, load64(device + 16) | 0x100000 | 0x20000);
  }
  if (!shader) return;
  const u32 block = shader + (vertex ? 872 : 40);
  if (vertex && block == 0) return;
  const u32 offset = gw_load32(block + 20);
  if (!offset) return;
  const u32 q = block + offset;
  const u32 dirty = device + (vertex ? 0 : 8);
  store64(dirty, load64(dirty) & ~load64(q + 0));
  if (load64(q + 8) != 0) store64(device + 32, load64(device + 32) | (1ull << 56));
  const u32 shadows = device + 1152;
  u32 at = q + 20;
  const u32 end = at + gw_load32(q + 16);
  if (at >= end) return;
  // Records {offset, count}: skip pairs until one with count 0...
  for (;;) {
    const u32 count = gw_load16(at + 2);
    at += 4;
    if (count == 0) break;
    at += 4;
    if (at >= end) break;
  }
  if (at >= end) return;
  // ...then copies until one with count 0...
  bool copies_done = false;
  for (;;) {
    const u32 count = gw_load16(at + 2), to = gw_load16(at);
    at += 4;
    if (count == 0) {
      copies_done = true;
      break;
    }
    const u32 bytes = count * 4;
    for (u32 b = 0; b < bytes; b += 4) gw_store32(shadows + to + b, gw_load32(at + b));
    at += bytes;
    if (at >= end) break;
  }
  if (!copies_done) return;
  // ...then and/or runs until one with count 0.
  while (at < end) {
    u32 count = gw_load16(at + 2);
    const u32 to = gw_load16(at);
    at += 4;
    if (count == 0) return;
    u32 dst = shadows + to;
    do {
      const u32 v = (gw_load32(dst) & gw_load32(at)) | gw_load32(at + 4);
      at += 8;
      gw_store32(dst, v);
      dst += 4;
      count = (count + 0xFFFE) & 0xFFFF;
    } while (count);
  }
}
}  // namespace

extern "C" void sub_820A68B0(PPCContext& ctx, uint8_t* base) { set_shader(ctx, base, true); }
extern "C" void sub_820A65F8(PPCContext& ctx, uint8_t* base) { set_shader(ctx, base, false); }

// SetViewport (guest 0x8209D8A0): r3 device, f1 x, f2 y, f3 width, f4
// height, f5 min z, f6 max z. With no render target (+12440, else the depth
// target +12456) nothing changes. The size is clamped to the target (its
// +36: width - 1 in bits 18-31, height - 1 in bits 3-17; when the device is
// tiling into the targets it was set up with, +13180/+13184), the viewport
// kept at +12648..+12672, the scissor set (sub_8209D288), and the viewport
// transform's shadows (+10504..+10524: x scale and offset, y scale and
// offset, z scale and offset) and their dirty bits (+16) written. The
// recompiled float conversions made it one of the costliest Direct3D
// functions on the Xbox.
namespace {
inline i32 truncate_double(double d) {  // fctiwz on a double
  if (d != d) return (i32)0x80000000;
  if (d >= 2147483647.0) return 0x7FFFFFFF;
  if (d <= -2147483648.0) return (i32)0x80000000;
  return (i32)d;
}
inline void store_float(u32 a, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  gw_store32(a, v);
}
}  // namespace

extern "C" void sub_8209D8A0(PPCContext& ctx, uint8_t* base) {
  const u32 dev = ctx.r3.u32;
  const double x = ctx.f1.f64, y = ctx.f2.f64, w = ctx.f3.f64, h = ctx.f4.f64, min_z = ctx.f5.f64,
               max_z = ctx.f6.f64;
  u32 surface = gw_load32(dev + 12440);
  if (!surface) surface = gw_load32(dev + 12456);
  if (!surface) return;
  const u8 flags = gw_load8(dev + 10940);
  bool tiling = false;
  if (flags & 0x10) {
    tiling = true;
  } else if (flags & 0x20) {
    tiling = true;
    for (u32 k = 0; k < 5 && tiling; ++k) {
      const u32 mine = gw_load32(dev + 12440 + k * 4), set_up = gw_load32(dev + 12728 + k * 4);
      tiling = mine == set_up || mine == 0;
    }
  }
  i32 limit_w, limit_h;
  if (tiling) {
    limit_w = (i32)gw_load32(dev + 13180);
    limit_h = (i32)gw_load32(dev + 13184);
  } else {
    const u32 info = gw_load32(surface + 36);
    limit_h = (i32)((info >> 3) & 0x7FFF) + 1;
    limit_w = (i32)((info >> 18) & 0x3FFF) + 1;
  }
  const i32 x0 = truncate_double(x), y0 = truncate_double(y);
  i32 right = truncate_double(w) + x0;
  if (right >= limit_w) right = limit_w;
  i32 bottom = truncate_double(h) + y0;
  if (bottom >= limit_h) bottom = limit_h;
  i32 width = right - x0, height = bottom - y0;
  if (width < 0 || height < 0) width = height = 0;
  store_float(dev + 12648, (float)x);
  store_float(dev + 12652, (float)y);
  store_float(dev + 12664, (float)min_z);
  store_float(dev + 12668, (float)max_z);
  gw_store32(dev + 12672, 0);
  const float fw = (float)width, fh = (float)height;
  store_float(dev + 12656, fw);
  store_float(dev + 12660, fh);
  ctx.r1.u64 = ctx.r1.u32 - 176;  // the original's frame
  ctx.r3.u64 = dev;
  ctx.r4.u64 = dev + 12676;
  sub_8209D288(ctx, base);
  ctx.r1.u64 = ctx.r1.u32 + 176;
  const u32 half_bits = gw_load32(0x82000000 + 1828);
  float half;
  memcpy(&half, &half_bits, 4);
  store_float(dev + 10524, (float)min_z);
  store_float(dev + 10520, (float)(max_z - min_z));
  const float sx = fw * half, sy = fh * half;
  store_float(dev + 10504, sx);
  store_float(dev + 10508, (float)((double)sx + x));
  store_float(dev + 10512, -sy);
  store_float(dev + 10516, (float)((double)sy + y));
  gw_store64(dev + 16, gw_load64(dev + 16) | 0x07E00000ull);
}
