// NV2A renderer (pbkit), first stage: the title screen's scene layers.
//
// The game's draws are recognised by a fingerprint of their vertex shader.
// For the few simple ones (position through a 4x4 matrix, optional texture
// coordinates and colour) the vertices are read from guest memory and
// transformed on the CPU, then drawn with a pass-through vertex program and a
// texture-times-colour combiner. Everything else (the glow chain, resolves,
// clears, the grid) is skipped for now.
#include "runtime.h"
#include "dxt.h"
#include "xenos_vs.h"

#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <xmmintrin.h>

#ifndef MASK
#define MASK(mask, value) (((value) << (__builtin_ffs(mask) - 1)) & (mask))
#endif

// pbkit's pb_push1/pb_push2/pb_push are function calls (with checks only in
// its debug build); the renderer makes thousands a frame, so here they are
// inline writes of the same words: method header (count << 18, subchannel
// 0), then the parameters. Also used by the fp20compiler output included
// below.
namespace {
inline u32* gw_push1(u32* p, u32 method, u32 a) {
  p[0] = 1u << 18 | method;
  p[1] = a;
  return p + 2;
}
inline u32* gw_push2(u32* p, u32 method, u32 a, u32 b) {
  p[0] = 2u << 18 | method;
  p[1] = a;
  p[2] = b;
  return p + 3;
}
inline void gw_push(u32* p, u32 method, u32 count) { p[0] = count << 18 | method; }
}  // namespace
#define pb_push1(p, method, a) gw_push1((p), (method), (a))
#define pb_push2(p, method, a, b) gw_push2((p), (method), (a), (b))
#define pb_push(p, method, count) gw_push((p), (method), (count))

namespace {

bool g_ready = false;
}  // namespace

volatile bool gw_nv2a_active = false;  // the status page stops drawing
// Where the command processor thread is in the renderer: 0 idle, 1 pb_reset,
// 2 drawing, 3 waiting for the GPU, 4 waiting for the swap.
volatile u32 gw_nv2a_stage = 0;

namespace {
bool g_in_frame = false;
// Per-frame vertex arena, 20 bytes a vertex.
constexpr u32 kArenaBytes = 1024 * 1024;  // (the menus' and game over's screens: up to 860 KiB a frame)
u8* g_arena;
u32 g_arena_used;  // bytes
// Push buffer blocks are batched: every pb_end makes pbkit flush the CPU's
// write-combining buffer and wait for it through a GPU register, and the
// renderer used to close a block for each small state change (thousands a
// frame). blk_begin/blk_end keep one block open across them and close it
// once it holds kBlockWords words;
// blk_flush closes it before any pbkit call
// that opens its own (targets, resets, text, the flip).
u32* g_block;
u32* g_block_start;
// Each pb_end makes pbkit flush the write-combining buffer and wait for the
// GPU to see it through a register (about a tenth of the command processor's
// time on the Xbox with blocks of 32 words), so blocks are long. The GPU
// sees a block when it closes; the CPU is the bottleneck, not the GPU.
constexpr long kBlockWords = 512;

u32* blk_begin() {
  if (!g_block) g_block = g_block_start = pb_begin();
  return g_block;
}

void blk_flush() {
  if (!g_block) return;
  pb_end(g_block);
  g_block = nullptr;
}

void blk_end(u32* p) {
  g_block = p;
  if (p - g_block_start >= kBlockWords) blk_flush();
}

// Draw with translated NV2A vertex programs (else the CPU interpreter).
bool g_use_gpu_vertex = true;
float g_scale_x, g_scale_y;
// Viewport scale for the current target: g_scale_x/y for the screen, the
// colour field's own size for its splats (see the colour field below).
float g_target_scale_x, g_target_scale_y;
float g_target_row;  // added to y: the colour field's work area (its splats), or the letterbox's top bar
float g_screen_row;  // the letterbox's top bar (rows), 0 when the picture fills the screen
// Our own glow over each frame (on unless make GLOW=n; see apply_glow).
#ifdef GW_GLOW
constexpr bool kGlow = true;
#else
constexpr bool kGlow = false;
#endif
constexpr float kGlowStrength = 0.7f;
constexpr u32 kGlowProgramSlot = 128;  // nv2a_glow_vs.vs.cg (after the translated programs)
u32 g_frames;
// This frame's texture conversions and failures, and vertex arena refills
// (the flicker detector in gw_nv2a_swap reports them with a frame that lost
// draws).
u32 g_frame_conversions, g_frame_conversion_failures, g_frame_arena_refills;
// The glow's and the colour field's work areas: rows of pbkit's extra
// buffers (chosen in gw_nv2a_init). 0 the glow's quarter-size picture, 1 its
// blur, 2-4 the colour field's three regions (each 160x116 at most).
struct WorkArea {
  int buffer;  // pbkit extra buffer
  u32 row;     // first row
};
WorkArea g_work[5];

struct Texture {
  u32 key;  // gpu address ^ size ^ format
  u32* pixels;
  u32 format;
  u32 filter;  // NV097_SET_TEXTURE_FILTER
  float u_scale = 1, v_scale = 1;  // the image's share of the (padded) texture
  u32 address = 0x00030303;        // NV097_SET_TEXTURE_ADDRESS (clamp)
  u32 last_used = 0;               // the frame it was last drawn with (g_frames)
  u32 bytes = 0;
  bool source_released = false;    // the 360 original's memory given back: never evicted (it could
                                   // not be converted again: the original reads as zeros now)
};
Texture g_textures[32];
Texture g_white;
// The glowing lines' (and the vector font's) brightness across a stroke: the
// game's pixel shaders (ED8C5D26 and 30398AC8 for the lines, CABF22DB for
// their glow outlines) multiply the colour by (1 - |u|)^4, u running -1..1 across the
// stroke, so a stroke is a thin bright core fading to nothing at its edges.
// The NV2A's combiners cannot raise to a power per pixel, so the curve is a
// 64-texel texture read with u: coordinate u / 2, wrapping (texel s holds
// u = 2s for s < 1/2, 2s - 2 above), through the texture-times-colour
// combiner. Drawn flat (no curve) the strokes were solid bars.
Texture g_falloff;
constexpr u32 kFalloffSize = 64;

u32 swizzle_offset(u32 x, u32 y, u32 w, u32 h) {
  u32 out = 0, dest = 1;
  for (u32 bit = 1; bit < w || bit < h; bit <<= 1) {
    if (bit < w) {
      if (x & bit) out |= dest;
      dest <<= 1;
    }
    if (bit < h) {
      if (y & bit) out |= dest;
      dest <<= 1;
    }
  }
  return out;
}

u32 log2_ceil(u32 v) {
  u32 l = 0;
  while ((1u << l) < v) ++l;
  return l;
}

// Xenos 2D tiling (ReXGlue texture/conversion.cpp).
u32 tiled_row(u32 y, u32 width, u32 log2_bpp) {
  u32 macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  u32 micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
}

u32 tiled_column(u32 x, u32 y, u32 log2_bpp, u32 base) {
  u32 macro = (x / 32) << (log2_bpp + 7);
  u32 micro = (x & 7) << log2_bpp;
  u32 offset = base + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

u32 swap_endian(u32 v, u32 endian) {
  switch (endian & 3) {
    case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}

u32 nv_format(u32 w, u32 h, u32 colour = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, u32 levels = 1) {
  return 1 | (2 << 4) | NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE | (colour << 8) | (levels << 16) |
         (log2_ceil(w) << 20) | (log2_ceil(h) << 24);
}

// Converts a k_8_8_8_8 texture to an NV2A texture, cached by address. The
// image is kept at its own size, pixel for pixel: padded out to powers of
// two (its edge pixels repeated) with the draw's texture coordinates scaled
// to the image's share (u_scale, v_scale), not resampled (resampling to a
// power of two dropped and doubled rows and columns of the fonts: uneven,
// misshapen letters). Sides over 2048 are scaled down. Mip levels only as
// the game has them (none of its textures do: the 360 samples them at full
// detail; made-up mipmaps with trilinear filtering washed thin strokes out).
// Grey ones (the fonts) as swizzled A8Y8, exact; other colour ones up to
// 256x256 as swizzled A8R8G8B8, larger ones (title art) as DXT5, which keeps
// their full resolution in a quarter of the memory.
// Frees the converted texture drawn with longest ago (not this frame, and
// not one whose original is gone); false if there is none. For a conversion
// that found no slot or no memory: the draw goes ahead instead of being
// skipped for the frame.
bool evict_oldest_texture() {
  Texture* oldest = nullptr;
  for (Texture& t : g_textures) {
    if (!t.pixels || t.source_released || t.last_used == g_frames) continue;
    if (!oldest || t.last_used < oldest->last_used) oldest = &t;
  }
  if (!oldest) return false;
  gw_log("nv2a: texture %08X given back (%u KiB) to make room\n", oldest->key, oldest->bytes / 1024);
  MmFreeContiguousMemory(oldest->pixels);
  oldest->pixels = nullptr;
  return true;
}

Texture* texture_from_fetch(const u32* f) {
  u32 format = f[1] & 0x3F;
  // 6: k_8_8_8_8; 2: k_8 (one byte, e.g. the grid colour field's light spot).
  if (format != 6 && format != 2) {
    static u32 reported[8];
    bool seen = false;
    for (u32 r : reported) seen |= r == (format | 0x100);
    if (!seen) {
      for (u32& r : reported) {
        if (!r) {
          r = format | 0x100;
          break;
        }
      }
      gw_log("nv2a: texture format %u not converted yet (%ux%u at %08X)\n", format, (f[2] & 0x1FFF) + 1,
             ((f[2] >> 13) & 0x1FFF) + 1, f[1] & 0xFFFFF000);
    }
    return nullptr;
  }
  u32 base = f[1] & 0xFFFFF000;
  u32 width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  u32 key = base ^ (width << 1) ^ (height << 17) ^ (format << 26);
  for (Texture& t : g_textures) {
    if (t.pixels && t.key == key) {
      t.last_used = g_frames;
      return &t;
    }
  }
  Texture* slot = nullptr;
  for (int attempt = 0; attempt < 2 && !slot; ++attempt) {
    for (Texture& t : g_textures) {
      if (!t.pixels) {
        slot = &t;
        break;
      }
    }
    if (!slot && !evict_oldest_texture()) break;
  }
  if (!slot) {
    ++g_frame_conversion_failures;
    static u32 told = 0;
    if (told++ < 8) gw_log("nv2a: texture %08X %ux%u not converted: all %u slots in use\n", base, width, height,
                           (u32)(sizeof(g_textures) / sizeof(g_textures[0])));
    return nullptr;
  }
  u32 tw = 1u << log2_ceil(width), th = 1u << log2_ceil(height);
  if (tw > 2048) tw = 2048;
  if (th > 2048) th = 2048;
  const bool scale_x = tw < width, scale_y = th < height;  // (only past 2048)
  const u32 guest_mips = (f[4] >> 6) & 15;  // the game's own mip levels beyond the first
  bool tiled = f[0] >> 31;
  u32 pitch = ((f[0] >> 22) & 0x1FF) * 32;
  u32 endian = (f[1] >> 6) & 3;
  const u8* src = gw_host(gw_gpu_to_guest(base));
  // The source pixel for (x, y) of the converted texture (nearest).
  // k_8: the fetch constant's swizzle (3 bits for each of x, y, z, w: 0 the
  // byte, 1-2 absent channels (0), 3 absent alpha (1), 4 zero, 5 one) gives
  // the A8R8G8B8 texel; the endian swap moves bytes within each word.
  const u32 swizzle = (f[3] >> 1) & 0xFFF;
  const u32 byte_flip = endian == 1 ? 1 : endian == 2 ? 3 : endian == 3 ? 2 : 0;
  auto texel = [&](u32 x, u32 y) {
    // Pixel for pixel; the padding repeats the last column and row.
    const u32 sx = scale_x ? x * width / tw : x < width ? x : width - 1;
    const u32 sy = scale_y ? y * height / th : y < height ? y : height - 1;
    if (format == 2) {
      const u32 offset = tiled ? tiled_column(sx, sy, 0, tiled_row(sy, pitch, 0)) : sy * pitch + sx;
      const u32 v = src[offset ^ byte_flip];
      auto channel = [&](u32 c) -> u32 {
        const u32 sel = (swizzle >> (3 * c)) & 7;
        return sel == 0 ? v : sel == 3 || sel == 5 ? 255 : 0;
      };
      return channel(3) << 24 | channel(0) << 16 | channel(1) << 8 | channel(2);
    }
    const u32 offset = tiled ? (tiled_column(sx, sy, 2, tiled_row(sy, pitch, 2)) >> 2) * 4 : (sy * pitch + sx) * 4;
    u32 raw;
    memcpy(&raw, src + offset, 4);
    return swap_endian(raw, endian);
  };
  // Grey images (the fonts: white glyphs, coloured by the vertices) are kept
  // as 16-bit alpha + grey, exact and half the size of 32-bit; DXT5's 4x4
  // blocks smear the edges of small letters. Within 2 levels of grey counts.
  bool grey = true;
  for (u32 y = 0; y < th && grey; ++y) {
    for (u32 x = 0; x < tw; ++x) {
      const u32 v = texel(x, y);
      const int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
      if (r - g > 2 || g - r > 2 || b - g > 2 || g - b > 2) {
        grey = false;
        break;
      }
    }
  }
  const bool compressed = !grey && (tw > 256 || th > 256) && tw >= 4 && th >= 4;
  // Mipmaps, each level a 2x2 average of the one above: the game draws its
  // text and art smaller than the textures (1280-wide art on a 640-wide
  // screen), and without them the minified glyphs alias. Down to 4 texels a
  // side for DXT5 (one block), 1 otherwise.
  const u32 smallest = compressed ? 4 : 1;
  u32 levels = 1, bytes = 0;
  for (u32 w = tw, h = th;; w >>= 1, h >>= 1) {
    bytes += compressed ? w * h : grey ? w * h * 2 : w * h * 4;
    if (levels > guest_mips || w / 2 < smallest || h / 2 < smallest || levels == 10) break;
    ++levels;
  }
  bytes = (bytes + 3) & ~3u;
  u32* pixels = (u32*)MmAllocateContiguousMemoryEx(bytes, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  while (!pixels && evict_oldest_texture())
    pixels = (u32*)MmAllocateContiguousMemoryEx(bytes, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (!pixels) {
    ++g_frame_conversion_failures;
    static u32 told = 0;
    if (told++ < 8) {
      gw_log("nv2a: texture %08X %ux%u not converted: no memory for %u KiB in one piece\n", base, width, height,
             bytes / 1024);
      gw_log_memory("texture conversion failed");
    }
    return nullptr;
  }
  // Writes one level (w x h) from get(x, y) at 'out'; returns the bytes used.
  auto write_level = [&](u8* out, u32 w, u32 h, auto&& get) {
    if (compressed) {
      for (u32 by = 0; by < h; by += 4) {
        for (u32 bx = 0; bx < w; bx += 4, out += 16) {
          u32 block[16];
          for (u32 i = 0; i < 16; ++i) block[i] = get(bx + (i & 3), by + (i >> 2));
          gw_dxt5_block(block, out);
        }
      }
      return w * h;
    }
    if (grey) {
      // A8Y8: grey in the low byte, alpha in the high one.
      u16* texels = (u16*)out;
      for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
          const u32 v = get(x, y);
          texels[swizzle_offset(x, y, w, h)] = (u16)(((v >> 16) & 0xFF00) | ((v >> 8) & 0xFF));
        }
      }
      return w * h * 2;
    }
    u32* texels = (u32*)out;
    for (u32 y = 0; y < h; ++y) {
      for (u32 x = 0; x < w; ++x) texels[swizzle_offset(x, y, w, h)] = get(x, y);
    }
    return w * h * 4;
  };
  auto average = [](u32 a, u32 b, u32 c, u32 d) {
    u32 out = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
      const u32 sum = ((a >> shift) & 255) + ((b >> shift) & 255) + ((c >> shift) & 255) + ((d >> shift) & 255);
      out |= ((sum + 2) / 4) << shift;
    }
    return out;
  };
  u8* out = (u8*)pixels;
  out += write_level(out, tw, th, texel);
  if (levels > 1) {
    // Level 1 from the source, then each level from the one before, kept in
    // a linear scratch copy (a quarter of level 0).
    u32 w = tw / 2, h = th / 2;
    u32* level = (u32*)malloc(w * h * 4);
    if (!level) {
      levels = 1;
    } else {
      for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
          level[y * w + x] =
              average(texel(2 * x, 2 * y), texel(2 * x + 1, 2 * y), texel(2 * x, 2 * y + 1), texel(2 * x + 1, 2 * y + 1));
        }
      }
      for (u32 n = 1; n < levels; ++n) {
        out += write_level(out, w, h, [&](u32 x, u32 y) { return level[y * w + x]; });
        if (n + 1 == levels) break;
        const u32 nw = w / 2, nh = h / 2;
        for (u32 y = 0; y < nh; ++y) {
          for (u32 x = 0; x < nw; ++x) {
            level[y * nw + x] = average(level[2 * y * w + 2 * x], level[2 * y * w + 2 * x + 1],
                                        level[(2 * y + 1) * w + 2 * x], level[(2 * y + 1) * w + 2 * x + 1]);
          }
        }
        w = nw;
        h = nh;
      }
      free(level);
    }
  }
  slot->key = key;
  slot->pixels = pixels;
  slot->last_used = g_frames;
  slot->bytes = bytes;
  slot->source_released = false;
  ++g_frame_conversions;
  slot->format = nv_format(tw, th,
                           compressed ? NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8
                           : grey     ? NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8
                                      : NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8,
                           levels);
  slot->filter = levels > 1 ? 0x02062000 : 0x02022000;  // trilinear with mipmaps, else bilinear
  slot->u_scale = scale_x ? 1.0f : (float)width / (float)tw;
  slot->v_scale = scale_y ? 1.0f : (float)height / (float)th;
  gw_log("nv2a: texture %08X %ux%u%s -> %ux%u%s, %u levels\n", base, width, height, tiled ? " tiled" : "", tw, th,
         compressed ? " DXT5" : grey ? " grey" : "", levels);
  // Large images (the title screens) are only read by the GPU: once
  // converted, the 360-format original gives its memory back. A later touch
  // recommits it as zeros.
  u32 guest = gw_gpu_to_guest(base);
  if (gw_allocation_base(guest) == guest && gw_virtual_size(guest) >= 0x40000) {
    gw_virtual_decommit(guest, gw_virtual_size(guest));
    slot->source_released = true;
  }
  return slot;
}

// Colour as D3DCOLOR (A8R8G8B8); HDR values above 1 saturate.
u32 pack_color(const float* c) {
  auto byte = [](float v) -> u32 { return v <= 0 ? 0 : v >= 1 ? 255 : (u32)(v * 255.0f + 0.5f); };
  return (byte(c[3]) << 24) | (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]);
}

float as_float(u32 bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

void begin_frame() {
  blk_flush();
  gw_nv2a_stage = 1;
  const u64 start = gw_wait_begin();
  // The GPU done with the previous frame before its vertex arena is written
  // again: pb_reset only waits until the GPU has read the commands, and the
  // previous frame's last draws (the menus' foreground, on the screens that
  // fill the arena) could still be reading their vertices when this frame's
  // first draws overwrote them (single frames without the foreground).
  pb_reset();
  while (pb_busy()) {
  }
  gw_wait_end(start, 0, "renderer: GPU finishing the previous frame");
  gw_nv2a_stage = 2;
  blk_flush();
  pb_target_back_buffer();
  pb_erase_depth_stencil_buffer(0, 0, pb_back_buffer_width(), pb_back_buffer_height());
  pb_fill(0, 0, pb_back_buffer_width(), pb_back_buffer_height(), 0);
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
  p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
  p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
  p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
  p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
  // 16-bit targets: dithered, so the glow's and the colour field's soft
  // gradients don't band.
  p = pb_push1(p, NV097_SET_DITHER_ENABLE, 1);
  blk_end(p);
  g_arena_used = 0;
  g_in_frame = true;
}

void bind_texture(const Texture* t) {
  u32* p = blk_begin();
  p = pb_push2(p, NV097_SET_TEXTURE_OFFSET, (u32)t->pixels & 0x03FFFFFF, t->format);
  p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, t->address);
  p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0x4003FFC0);
  p = pb_push1(p, NV097_SET_TEXTURE_FILTER, t->filter);
  blk_end(p);
}

// RB_BLENDCONTROL0 blend factors (Xenos BlendFactor) -> NV2A (GL) factors.
u32 blend_factor(u32 x) {
  static const u32 kFactors[17] = {
      0x0000, 0x0001, 0x0001, 0x0001,  // zero, one, (unused), (unused)
      0x0300, 0x0301, 0x0302, 0x0303,  // src colour, 1-src colour, src alpha, 1-src alpha
      0x0306, 0x0307, 0x0304, 0x0305,  // dst colour, 1-dst colour, dst alpha, 1-dst alpha
      0x8001, 0x8002, 0x8003, 0x8004,  // constant colour/alpha and inverses
      0x0308,                          // src alpha saturate
  };
  return x < 17 ? kFactors[x] : 1;
}

}  // namespace

// The display (CRTC) starts each line of the frame 'offset' x 8 bytes after
// the last: CR13 holds the offset's bits 0-7, CR19 bits 5-7 its bits 8-10,
// CR42 bit 6 its bit 11. At 720p (2560-byte lines) the TV showed the frame
// read at the wrong line length while the frame in memory was right, so it
// is set here from pbkit's pitch, and checked again every second
// (src/status.cpp). CR1F unlocks the extended registers (0x57); its lock is
// put back afterwards, as is the index register.
void gw_crtc_pitch() {
  static u32 pitch = 0;
  if (!pitch) pitch = pb_back_buffer_pitch();
  const u32 offset = pitch / 8;
  volatile u8* const index = (volatile u8*)0xFD6013D4;
  volatile u8* const data = (volatile u8*)0xFD6013D5;
  const u8 old_index = *index;
  *index = 0x1F;
  const u8 lock = *data;
  *data = 0x57;
  auto get = [&](u8 r) {
    *index = r;
    return (u8)*data;
  };
  auto put = [&](u8 r, u8 v) {
    *index = r;
    *data = v;
  };
  const u8 cr13 = get(0x13), cr19 = get(0x19), cr42 = get(0x42);
  const u8 want13 = (u8)(offset & 0xFF);
  const u8 want19 = (u8)((cr19 & 0x1F) | (((offset >> 8) & 7) << 5));
  const u8 want42 = (u8)((cr42 & ~0x40) | (((offset >> 11) & 1) << 6));
  const bool wrong = cr13 != want13 || cr19 != want19 || cr42 != want42;
  if (wrong) {
    put(0x13, want13);
    put(0x19, want19);
    put(0x42, want42);
  }
  put(0x1F, lock);
  *index = old_index;
  if (wrong)
    gw_log("crtc: line length was CR13 %02X CR19 %02X CR42 %02X, set for %u bytes\n", cr13, cr19, cr42, pitch);
}

void gw_nv2a_init() {
  u32 vs[] = {
#include "nv2a_vs.inl"
  };
  // 16-bit back buffers to match the video mode set in status.cpp (memory).
  pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false);
  // Work areas (see WorkArea) in as few of pbkit's extra buffers (each a whole
  // frame, 1.8 MB at 720p) as hold them: the glow's quarter-size picture and
  // its blurred copy, then the colour field's three 116-row bands, in one
  // buffer at 720p, the field in a second one at 480 lines.
  {
    const VIDEO_MODE mode = XVideoGetMode();
    const u32 quarter = (u32)mode.height / 4, band = 116;
    g_work[0] = {0, 0};
    g_work[1] = {0, quarter};
    const bool one = 2 * quarter + 3 * band <= (u32)mode.height;
    for (u32 k = 0; k < 3; ++k) g_work[2 + k] = one ? WorkArea{0, 2 * quarter + k * band} : WorkArea{1, k * band};
    pb_extra_buffers(one ? 1 : 2);
  }
  if (pb_init()) {
    gw_log("nv2a: pb_init failed\n");
    return;
  }
  gw_crtc_pitch();
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
  p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
               MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                   MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
                        NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
  blk_end(p);
  for (u32 i = 0; i < sizeof(vs) / 4; i += 4) {
    p = blk_begin();
    pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
    memcpy(p, vs + i, 16);
    p += 4;
    blk_end(p);
  }
  p = blk_begin();
#include "nv2a_ps.inl"
  blk_end(p);
  {
    u32 glow_vs[] = {
#include "nv2a_glow_vs.inl"
    };
    p = blk_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, kGlowProgramSlot);
    blk_end(p);
    for (u32 i = 0; i < sizeof(glow_vs) / 4; i += 4) {
      p = blk_begin();
      pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
      memcpy(p, glow_vs + i, 16);
      p += 4;
      blk_end(p);
    }
  }

  g_arena = (u8*)MmAllocateContiguousMemoryEx(kArenaBytes, 0, 0x03FFAFFF, 0,
                                               PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (!g_arena) {
    gw_log("nv2a: no memory for the vertex arena\n");
    return;
  }
  g_white.pixels = (u32*)MmAllocateContiguousMemoryEx(16, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  for (int i = 0; i < 4; ++i) g_white.pixels[i] = 0xFFFFFFFF;
  g_white.format = nv_format(2, 2);
  g_white.filter = 0x02022000;
  g_falloff.pixels = (u32*)MmAllocateContiguousMemoryEx(kFalloffSize * 4 * 4, 0, 0x03FFAFFF, 0,
                                                         PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (g_falloff.pixels) {
    for (u32 i = 0; i < kFalloffSize; ++i) {
      const float sc = ((float)i + 0.5f) / (float)kFalloffSize;
      const float u = sc < 0.5f ? 2 * sc : 2 * sc - 2;
      const float f = 1 - (u < 0 ? -u : u);
      const u32 b = (u32)(f * f * f * f * 255.0f + 0.5f);
      for (u32 y = 0; y < 4; ++y) g_falloff.pixels[swizzle_offset(i, y, kFalloffSize, 4)] = b << 24 | b << 16 | b << 8 | b;
    }
    g_falloff.format = nv_format(kFalloffSize, 4);
    g_falloff.filter = 0x02022000;  // bilinear
    g_falloff.address = 0x00030301;  // wrap across, clamp along
    g_falloff.u_scale = 0.5f;
    g_falloff.v_scale = 0.0f;
  }

  // The game's 1280x720 picture over the screen; on a 4:3 setting at 480
  // lines letterboxed: the full width, three quarters of the height, centred
  // (the bars are left black: each frame starts cleared).
  g_scale_x = (float)pb_back_buffer_width() / 1280.0f;
  g_scale_y = (float)pb_back_buffer_height() / 720.0f;
  if (gw_video_letterbox()) {
    g_scale_y *= 0.75f;
    g_screen_row = (float)pb_back_buffer_height() / 8;
  }
  g_target_scale_x = g_scale_x;
  g_target_scale_y = g_scale_y;
  g_target_row = g_screen_row;
  blk_flush();
  pb_show_front_screen();
  g_ready = true;
  gw_nv2a_active = true;
  gw_log("nv2a: ready, %ux%u\n", (u32)pb_back_buffer_width(), (u32)pb_back_buffer_height());
}

// Called for each DRAW_INDX(_2) with the current register file.
namespace {
// Per-frame census of vertex shaders (logged every 300 frames) to find the
// draws that make up whatever screen is showing.
struct CensusEntry {
  u32 hash, ps, draws, prim, vertices, gpu, cpu;  // gpu/cpu: vertices drawn by each path
  u32 strokes;  // draws given the line falloff (g_falloff)
  u32 drawn;    // draws that reached the NV2A
  u32 in_place;     // vertices drawn from a host mirror (gw_vertex_mirror)
  u32 arena_bytes;  // bytes this kind copied into the vertex arena
  u32 skip;  // why the last skipped draw was skipped (SkipReason), with its detail
  u32 skip_detail;
};
CensusEntry g_census[32];
CensusEntry g_census_last[32];  // the previous drawn frame's (the flicker detector)
// The order of this frame's draws that reached the NV2A, each kind (vertex
// and pixel shader) at its first draw, and the previous frame's: a kind that
// moved (a black menu panel after the menu's text) would hide what it now
// covers for that frame.
constexpr u32 kOrderMax = 24;
u32 g_order[kOrderMax][2], g_order_last[kOrderMax][2];
u32 g_order_count, g_order_last_count;
void note_order(u32 vs, u32 ps) {
  for (u32 i = 0; i < g_order_count; ++i)
    if (g_order[i][0] == vs && g_order[i][1] == ps) return;
  if (g_order_count < kOrderMax) {
    g_order[g_order_count][0] = vs;
    g_order[g_order_count][1] = ps;
    ++g_order_count;
  }
}
CensusEntry* g_census_entry;

enum SkipReason : u32 { kDrawn, kNotMainTarget, kPrimitive, kComputedUv, kTextureFormat };
const char* const kSkipNames[] = {"", "target", "primitive", "computed texture coordinates", "texture format"};

void note_skip(SkipReason reason, u32 detail) {
  if (!g_census_entry) return;
  g_census_entry->skip = reason;
  g_census_entry->skip_detail = detail;
}
}  // namespace

namespace {

// Vertex array setup: formats[a] / offsets[a] for the 16 attributes (format
// 0 = off). pbkit blocks are limited to 128 dwords.
void set_arrays(const u32* formats, const u32* offsets) {
  for (u32 attr = 0; attr < 16; attr += 4) {
    u32* p = blk_begin();
    for (u32 a = attr; a < attr + 4; ++a) {
      u32 format = formats[a] ? formats[a] : NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
      p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + a * 4, format);
      if (formats[a]) p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + a * 4, offsets[a]);
    }
    blk_end(p);
  }
}

void draw_arrays(u32 nv_prim, u32 count) {
  // DRAW_ARRAYS takes at most 256 vertices; 252 keeps quads and triangles
  // whole.
  for (u32 start = 0; start < count;) {
    u32 n = count - start > 252 ? 252 : count - start;
    u32* p = blk_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, nv_prim);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS, ((n - 1) << 24) | start);
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    blk_end(p);
    start += n;
  }
}

u32 g_program_start = 0xFFFFFFFF;  // transform program start slot in use
u32 g_loaded_key = 0;              // translated program loaded at slot 8

void set_program_start(u32 start) {
  if (g_program_start == start) return;
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, start);
  blk_end(p);
  g_program_start = start;
}

// Uploads constants (4 floats each) from slot first.
void upload_constants(u32 first, const u32* values, u32 count) {
  for (u32 done = 0; done < count;) {
    u32 n = count - done > 8 ? 8 : count - done;
    u32* p = blk_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 + first + done);
    pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, n * 4);
    memcpy(p, values + done * 4, n * 16);
    p += n * 4;
    blk_end(p);
    done += n;
  }
}

u32 float_bits(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  return u;
}

// Room for 'bytes' more in the vertex arena. When a busy frame fills it, the
// GPU finishes what it has been given and the arena starts again from the
// beginning: one stall instead of dropping draws or falling back to the CPU
// interpreter. pb_reset only waits until the GPU has read the commands, not
// until it has drawn them: the draws still in its queue read their vertices
// from the arena, so it also waits for the GPU to be idle before the arena is
// written again (without, the menus, the screens that fill it, lost their
// foreground for single frames).
bool arena_room(u32 bytes) {
  if (g_arena_used + bytes <= kArenaBytes) return true;
  if (bytes > kArenaBytes) return false;
  static u32 reported = 0;
  if (reported++ < 4) gw_log("nv2a: frame %u filled the vertex arena, waiting for the GPU\n", g_frames);
  blk_flush();
  const u64 start = gw_wait_begin();
  pb_reset();
  while (pb_busy()) {
  }
  ++g_frame_arena_refills;
  gw_wait_end(start, 0, "renderer: vertex arena full mid-frame");
  g_arena_used = 0;
  return true;
}

// Rectangle lists (Xenos primitive 8) give three corners per rectangle; the
// NV2A has no such primitive. The corner with the right angle is the one
// opposite the longest edge (by the first attribute, the position); the
// fourth corner is the sum of its two neighbours minus it, for every float
// attribute (exact for the affine vertex shaders of full-screen passes).
// Writes the four corners as a quad, in order around it.
void rect_to_quad(const u32* v[3], const XvpProgram* gp, u32* out) {
  const u32 words = gp->stride / 4, pos = gp->attrs[0].offset / 4;
  auto at = [&](int k, u32 w) { return as_float(v[k][w]); };
  auto dist2 = [&](int a, int b) {
    const float dx = at(a, pos) - at(b, pos), dy = at(a, pos + 1) - at(b, pos + 1);
    return dx * dx + dy * dy;
  };
  const float d01 = dist2(0, 1), d12 = dist2(1, 2), d02 = dist2(0, 2);
  const int r = d12 >= d01 && d12 >= d02 ? 0 : d02 >= d01 ? 1 : 2;  // the right-angle corner
  const int a = (r + 1) % 3, b = (r + 2) % 3;
  memcpy(out, v[a], gp->stride);
  memcpy(out + words, v[r], gp->stride);
  memcpy(out + 2 * words, v[b], gp->stride);
  u32* o = out + 3 * words;
  memcpy(o, v[r], gp->stride);  // non-float attributes: the right-angle corner's
  for (u32 n = 0; n < gp->attr_count; ++n) {
    const XvpProgram::Attribute& attr = gp->attrs[n];
    if (attr.format == 6) continue;
    for (u32 c = 0; c < attr.components; ++c) {
      const u32 w = attr.offset / 4 + c;
      o[w] = float_bits(at(a, w) + at(b, w) - at(r, w));
    }
  }
}

// Draws with the shader translated to an NV2A vertex program: the game's
// vertices are copied (byte-swapped) into the arena and read by the GPU.
// Vertices first .. first + count - 1 of the draw. 'rects': a rectangle
// list, drawn as quads. tex_w, tex_h scale the texture coordinates (a linear
// texture is addressed in texels). constant1: constant 1 for the programs
// that use it (xvp_get's line_i and scale_colour).
bool draw_on_gpu(const XvpProgram* gp, const u32* regs, u32 nv_prim, u32 first, u32 count, u32 source,
                 u32 index_base, u32 index_endian, bool index32, u32 vte, bool rects = false, float tex_w = 1,
                 float tex_h = 1, const float* constant1 = nullptr, const float* colour_scale = nullptr) {
  if (rects && (count % 3 || !gp->attr_count || gp->stride > 64)) return false;
  u32 out_count = rects ? count / 3 * 4 : count;
  u32 bytes = out_count * gp->stride;
  if (g_loaded_key != gp->key) {
    u32* p = blk_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 8);
    blk_end(p);
    for (u32 i = 0; i < gp->instructions; i += 16) {
      u32 n = gp->instructions - i > 16 ? 16 : gp->instructions - i;
      p = blk_begin();
      for (u32 k = 0; k < n; ++k) {
        pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
        memcpy(p, &gp->words[(i + k) * 4], 16);
        p += 4;
      }
      blk_end(p);
    }
    g_loaded_key = gp->key;
  }
  set_program_start(8);

  // Constants: 0-2 fixed, 3 viewport, 4+ the shader's.
  // c0.z, c0.w: texture coordinate scale (1 unless a linear texture is bound).
  static const float kFixed[12] = {0, 1, 1, 1,
                                   0.15915494f, 0.5f, 6.2831853f, 3.1415927f,
                                   1.2732395f, -0.40528473f, 0.225f, 1.5707963f};
  u32 c[64 * 4];
  memcpy(c, kFixed, sizeof(kFixed));
  c[2] = float_bits(tex_w);
  c[3] = float_bits(tex_h);
  // constant1: the grid lines' constants, in place of the sine constants
  // (their shader takes no sines); colour_scale: constant 64, the colour
  // scale of the colour field's splats and the glowing lines.
  if (constant1) memcpy(&c[4], constant1, 16);
  float vx_scale = as_float(regs[0x210F]), vx_off = as_float(regs[0x2110]);
  float vy_scale = as_float(regs[0x2111]), vy_off = as_float(regs[0x2112]);
  c[12] = float_bits((vte & 1 ? vx_scale : 1.0f) * g_target_scale_x);
  c[13] = float_bits((vte & 4 ? vy_scale : 1.0f) * g_target_scale_y);
  c[14] = float_bits((vte & 2 ? vx_off : 0.0f) * g_target_scale_x);
  c[15] = float_bits((vte & 8 ? vy_off : 0.0f) * g_target_scale_y + g_target_row);
  for (u32 i = 0; i < gp->const_count; ++i) memcpy(&c[(4 + i) * 4], &regs[0x4000 + gp->const_xenos[i] * 4], 16);
  upload_constants(0, c, 4 + gp->const_count);
  if (colour_scale) upload_constants(64, (const u32*)colour_scale, 1);

  u32 f0 = regs[0x4800 + gp->fetch_slot * 2], f1 = regs[0x4800 + gp->fetch_slot * 2 + 1];
  u32 stream = gw_gpu_to_guest(f0 & ~3u), endian = f1 & 3;
  // VGT_INDX_OFFSET: added to every vertex index, from the index buffer or
  // counted. The game draws its grid's lines in two draws from the same
  // stream, the second with offset 65528; without it the second drew the
  // first one's rows again and the grid's bottom quarter was missing.
  const u32 index_offset = regs[0x2102];
  u32 formats[16] = {}, offsets[16] = {};
  auto set_formats = [&](u32 base) {
    for (u32 a = 0; a < gp->attr_count; ++a) {
      const XvpProgram::Attribute& at = gp->attrs[a];
      formats[a] = at.format == 6
                       ? NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL | (4 << 4) | (gp->stride << 8)
                       : NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F | ((u32)at.components << 4) | (gp->stride << 8);
      offsets[a] = base + at.offset;
    }
    set_arrays(formats, offsets);
  };

  // The grid's lines: the game draws its line buffer of thin quads in two
  // draws (the second at index offset 65528). src/grid_native.cpp writes the
  // grid's points instead (GwGridLines in runtime.h), drawn here whole with
  // the first piece of the first draw as smoothed line strips (the other
  // pieces and draws do nothing): rows across from the array, columns down
  // by index. The program's second input (u, v, brightness, edge, of which it
  // uses the last two) is a constant per strip: (0, 0, colour, 0).
  if (source == 2 && !rects && gp->attr_count == 2 && gp->stride == 28 && gw_grid_line_host(stream)) {
    if (first + index_offset != 0 || !gw_grid_lines.host) return true;
    const u32 base = (u32)MmGetPhysicalAddress(gw_grid_lines.host);
    formats[0] = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F | (3 << 4) | (kGwGridLineVertex << 8);
    offsets[0] = base;
    set_arrays(formats, offsets);  // input 1 off: the constant below
    u32* p = blk_begin();
    p = pb_push1(p, NV097_SET_LINE_WIDTH, 12);  // 1.5 pixels, in eighths
    p = pb_push1(p, NV097_SET_LINE_SMOOTH_ENABLE, 1);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, 0x0302);  // source alpha: the smoothing's coverage
    blk_end(p);
    auto colour = [](u32* p, u32 n) {
      pb_push(p++, NV097_SET_VERTEX_DATA4F_M + 1 * 16, 4);
      *p++ = 0;
      *p++ = 0;
      *p++ = gw_grid_lines.colours[n & 7];
      *p++ = 0;
      return p;
    };
    constexpr u32 W = kGwGridLineCols;
    for (u32 r = 1; r + 1 < kGwGridLineRows; ++r) {
      p = blk_begin();
      p = colour(p, r);
      p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_LINE_STRIP);
      p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS, ((W - 1) << 24) | (r * W));
      p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
      blk_end(p);
    }
    constexpr u32 H = kGwGridLineRows;  // 89: 44 pairs of 16-bit indices and one 32-bit
    for (u32 c = 1; c + 1 < W; ++c) {
      p = blk_begin();
      p = colour(p, c);
      p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_LINE_STRIP);
      pb_push(p++, 0x40000000 | NV097_ARRAY_ELEMENT16, H / 2);
      for (u32 r = 0; r + 1 < H; r += 2) *p++ = (r * W + c) | ((r + 1) * W + c) << 16;
      p = pb_push1(p, NV097_ARRAY_ELEMENT32, (H - 1) * W + c);
      p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
      blk_end(p);
    }
    p = blk_begin();
    p = pb_push1(p, NV097_SET_LINE_SMOOTH_ENABLE, 0);
    blk_end(p);
    return true;
  }

  // Vertices the native code wrote into a host mirror in the NV2A's order
  // (gw_vertex_mirror: particles, stars): drawn where they are.
  bool mirrored = false;  // the stream starts in a mirror but runs past it
  if (source == 2 && !rects) {
    const u32 start = stream + (first + index_offset) * gp->stride;
    if (u8* host = gw_vertex_mirror(start, count * gp->stride)) {
      set_formats((u32)MmGetPhysicalAddress(host));
      draw_arrays(nv_prim, count);
      if (g_census_entry) g_census_entry->in_place += count;
      return true;
    }
    // Starting in a mirror and running past it (the busiest frames'
    // particles): the mirrored part drawn in place, in whole primitives, and
    // only the rest copied below.
    if (u8* host = gw_vertex_mirror(start, gp->stride)) {
      const u32 whole = nv_prim == NV097_SET_BEGIN_END_OP_QUADS       ? 4
                      : nv_prim == NV097_SET_BEGIN_END_OP_TRIANGLES   ? 3
                      : nv_prim == NV097_SET_BEGIN_END_OP_LINES       ? 2
                      : nv_prim == NV097_SET_BEGIN_END_OP_POINTS      ? 1
                                                                      : 0;
      const u32 inside = whole ? gw_vertex_mirror_room(start) / gp->stride / whole * whole : 0;
      if (inside && inside < count) {
        set_formats((u32)MmGetPhysicalAddress(host));
        draw_arrays(nv_prim, inside);
        if (g_census_entry) g_census_entry->in_place += inside;
        first += inside;
        count -= inside;
        out_count = count;
        bytes = count * gp->stride;
      }
      mirrored = gw_vertex_mirror(stream + (first + index_offset) * gp->stride, gp->stride) != nullptr;
    }
  }

  // Otherwise the vertices are copied into the arena, in draw order, words
  // swapped to host order.
  if (!arena_room(bytes)) return false;
  u32 words = gp->stride / 4;
  u32* out = (u32*)(g_arena + g_arena_used);
  u32 corners[3][16];  // a rectangle's three corners, host order
  // The usual draw: consecutive vertices, 32-bit words byte-reversed. One
  // straight copy, reading ahead: the game wrote the vertices earlier in the
  // frame and they have left the cache, so each line would otherwise wait
  // on memory (most of the command processor's time on the Xbox).
  if (source == 2 && !rects && endian == 2 && !mirrored) {
    const u32* in = (const u32*)gw_host(stream + (first + index_offset) * gp->stride);
    const u32 total = count * words;
    for (u32 k = 0; k < total; ++k) {
      if ((k & 7) == 0) _mm_prefetch((const char*)(in + k + 96), _MM_HINT_NTA);
      out[k] = __builtin_bswap32(in[k]);
    }
    count = 0;  // done
  }
  for (u32 i = 0; i < count; ++i) {
    u32 index = first + i;
    if (source == 0) {
      if (index32) {
        u32 raw;
        memcpy(&raw, gw_host(index_base + index * 4), 4);
        index = swap_endian(raw, index_endian);
      } else {
        u16 h;
        memcpy(&h, gw_host(index_base + index * 2), 2);
        index = index_endian ? (u16)((h << 8) | (h >> 8)) : h;
      }
    }
    const u32 at = stream + (index + index_offset) * gp->stride;
    const u32* in = (const u32*)gw_host(at);
    _mm_prefetch((const char*)in + 4 * gp->stride, _MM_HINT_NTA);
    u32* dst = rects ? corners[i % 3] : out;
    const u8* host = mirrored ? gw_vertex_mirror(at, gp->stride) : nullptr;
    if (host) {
      memcpy(dst, host, gp->stride);  // already in the NV2A's order
    } else if (endian == 2) {
      for (u32 k = 0; k < words; ++k) dst[k] = __builtin_bswap32(in[k]);
    } else {
      for (u32 k = 0; k < words; ++k) dst[k] = swap_endian(in[k], endian);
    }
    if (!rects) {
      out += words;
    } else if (i % 3 == 2) {
      const u32* v[3] = {corners[0], corners[1], corners[2]};
      rect_to_quad(v, gp, out);
      out += 4 * words;
    }
  }

  set_formats(((u32)g_arena & 0x03FFFFFF) + g_arena_used);
  g_arena_used += (bytes + 15) & ~15u;
  if (g_census_entry) g_census_entry->arena_bytes += (bytes + 15) & ~15u;
  draw_arrays(nv_prim, out_count);
  return true;
}

// --- Glow --------------------------------------------------------------------------------
// The game builds its glow from about 80 passes over float render targets
// (see TODO.md). Until those are reproduced, a glow of our
// own is added at the end of each frame (make GLOW=n leaves it out): the
// finished picture shrunk to a quarter
// (each pixel the average of 4x4), blurred across and down (8 texels each
// way), and added back over the picture. It uses pbkit's two extra buffers
// (640x480, 16-bit: 1.2 MB) and the four-texture sum combiner
// (nv2a_sum_ps.ps.cg).

// A linear R5G6B5 surface as the texture of a unit, addressed in texels.
void bind_linear(u32 unit, const void* pixels, u32 width, u32 height, u32 pitch) {
  const u32 o = unit * 64;
  u32* p = blk_begin();
  p = pb_push2(p, NV097_SET_TEXTURE_OFFSET + o, (u32)MmGetPhysicalAddress((PVOID)pixels),
               1 | (2 << 4) | NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE |
                   (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5 << 8) | (1 << 16));
  p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS + o, 0x00030303);  // clamp
  p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + o, 0x4003FFC0);
  p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1 + o, pitch << 16);
  p = pb_push1(p, NV097_SET_TEXTURE_FILTER + o, 0x02022000);  // bilinear
  p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT + o, (width << 16) | height);
  blk_end(p);
}

// Combiner constants for the four-texture sum, where fp20compiler put them
// (nv2a_sum_ps.inl; each stage keeps its own factors):
//   stage 0: t0 * factor0 + t1 * factor1
//   stage 1: previous + t2 * factor0   (its D source is 1, factor 0)
//   stage 2: previous + t3 * factor0
// Weights 2 and 3 used to go to factor 1 of stages 1 and 2, which nothing
// reads: on hardware those factors stayed 0 and the glow came out at an
// eighth of its strength (invisible); XEMU's leftovers made bright blocks.
// The unused factors are cleared so nothing is left over from earlier.
void set_sum_weights(const float w[4]) {
  auto colour = [](float v) {
    const u32 b = v <= 0 ? 0 : v >= 1 ? 255 : (u32)(v * 255.0f + 0.5f);
    return b << 24 | b << 16 | b << 8 | b;
  };
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR0 + 0 * 4, colour(w[0]));
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + 0 * 4, colour(w[1]));
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR0 + 1 * 4, colour(w[2]));
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + 1 * 4, 0);
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR0 + 2 * 4, colour(w[3]));
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + 2 * 4, 0);
  blk_end(p);
}

// The GPU finishes every earlier draw before the next command: needed on
// hardware between drawing a surface and reading it as a texture, and before
// pbkit moves the render target (it rewrites the target's DMA object, which
// draws still in flight would otherwise land in).
void wait_for_idle() {
  u32* p = blk_begin();
  p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
  blk_end(p);
  blk_flush();
}

u8* work_pixels(int area) {
  return (u8*)pb_extra_buffer(g_work[area].buffer) + g_work[area].row * pb_back_buffer_pitch();
}

// Render into pbkit's extra buffer 'index' (-1: the back buffer), ordered
// after everything drawn so far.
void glow_target(int index) {
  wait_for_idle();
  if (index < 0) {
    pb_target_back_buffer();
  } else {
    pb_target_extra_buffer(index);
  }
}

void use_sum_combiner() {
  u32* p = blk_begin();
#include "nv2a_sum_ps.inl"
  p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
               MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_EACH_STAGE) |
                   MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_EACH_STAGE) |
                   MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 3));
  blk_end(p);
}

// Which combiner setup is loaded, so draws switch only when it changes.
enum Combiner { kTextureTimesColour, kGridLine, kStrokeCore, kStrokeGlow, kOther };
Combiner g_combiner = kTextureTimesColour;

void use_default_combiner() {
  if (g_combiner == kTextureTimesColour) return;
  u32* p = blk_begin();
#include "nv2a_ps.inl"
  for (u32 unit = 1; unit < 4; ++unit) p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + unit * 64, 0);
  blk_end(p);
  g_combiner = kTextureTimesColour;
}

// The grid lines' pixel shader (B16AAC17) is
//   colour = min((field * brightness)^2, 1) * (1 - |edge|)^3
// with the colour field (below) sampled at the line's place and edge running
// from -1 to 1 across the thin quad of each line. The vertex program
// (xvp_get's line_i) gives texture coordinate 0 into the field, diffuse rgb =
// brightness / 2 (constant 1 below) and diffuse alpha a = edge / 2 + 1/2.
// The field is stored divided by kFieldScale (4), so field * brightness =
// 8 * t0 * diffuse = x; the combiners, each clamping to 1 (harmless: x >= 1
// saturates the result anyway):
//   stage 0: spare1.rgb = 4 (t0 + floor) diffuse = x / 2      spare0.a = a
//   stage 1: spare1.rgb = 4 spare1^2  = x^2         spare0.a = 2 (a < 1/2 ? a : 1 - a) = 1 - |edge|
//            (the mux picks CD when spare0's alpha is at least 1/2)
//   stage 2:                                         spare1.a = spare0.a^2
//   stage 3:                                         spare0.a = spare1.a * spare0.a = (1 - |edge|)^3
//   stage 4: spare0.rgb = spare1.rgb * spare0.a
// and the final combiner outputs spare0 (alpha too, for blends that use it).
// floor (combiner factor 0): a dim blue under the field, the plain grid
// colour of before (0.3, 0.28, 0.85) / kFieldScale. The field comes out
// dimmer than the game's (smaller blurs, no movement with the distortion),
// and without it the grid went dark away from explosions.
// rgb 0.4 rather than 0.5: the smoothed 1.5-pixel lines (gw_grid_line_host)
// carry more light than the game's soft-edged quads did, so the brightness
// is taken down by a fifth (the squared result by about a third).
const float kLineConstant1[4] = {0.4f, 0.4f, 0.4f, 0.5f};
constexpr u32 kGridFloor = 0x00131236;  // A8R8G8B8

void use_line_combiner() {
  if (g_combiner == kGridLine) return;
  // Combiner registers: 0 zero, 4 diffuse, 8 texture 0, 0xC spare0, 0xD
  // spare1. Input mappings: 0 as is, 1 inverted (1 - x); 1 is zero inverted.
  constexpr u32 kZero = 0, kDiffuse = 4, kTexture0 = 8, kSpare0 = 0xC, kSpare1 = 0xD, kInvert = 1;
  auto alpha_in = [](u32 a, u32 a_map, u32 b, u32 b_map, u32 c, u32 c_map, u32 d, u32 d_map) {
    return MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, a) | MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) |
           MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, a_map) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, b) |
           MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, b_map) |
           MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, c) | MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) |
           MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, c_map) | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, d) |
           MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, d_map);
  };
  auto alpha_out = [](u32 ab, u32 sum, bool mux, u32 op) {
    return MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, ab) | MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, sum) |
           MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, mux ? 1 : 0) | MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, op);
  };
  // Colour: A * B into spare1, times 4.
  auto colour_in = [](u32 a, u32 a_map, u32 b, u32 b_map) {
    return MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, a) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, a_map) |
           MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, b) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, b_map);
  };
  const u32 times4 = MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, kSpare1) |
                     MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY2);
  // Stage 0: t0 * diffuse + floor * diffuse into spare1, times 4.
  constexpr u32 kFactor0 = 1;
  const u32 field_in = colour_in(kTexture0, 0, kDiffuse, 0) | MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, kFactor0) |
                       MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, kDiffuse);
  const u32 field_out = MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, kSpare1) |
                        MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY2);
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR0, kGridFloor);
  p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
  p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
               MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE));
  blk_end(p);
  // Stages 0-3: {colour in, colour out, alpha in, alpha out}.
  const u32 stages[4][4] = {
      {field_in, field_out, alpha_in(kDiffuse, 0, kZero, kInvert, kZero, 0, kZero, 0),
       alpha_out(kSpare0, 0, false, 0)},
      {colour_in(kSpare1, 0, kSpare1, 0), times4,
       alpha_in(kDiffuse, 0, kZero, kInvert, kDiffuse, kInvert, kZero, kInvert),
       alpha_out(0, kSpare0, true, NV097_SET_COMBINER_ALPHA_OCW_OP_SHIFTLEFTBY1)},
      {0, 0, alpha_in(kSpare0, 0, kSpare0, 0, kZero, 0, kZero, 0), alpha_out(kSpare1, 0, false, 0)},
      {0, 0, alpha_in(kSpare1, 0, kSpare0, 0, kZero, 0, kZero, 0), alpha_out(kSpare0, 0, false, 0)},
  };
  for (u32 stage = 0; stage < 4; ++stage) {
    p = blk_begin();
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + stage * 4, stages[stage][0]);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + stage * 4, stages[stage][1]);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + stage * 4, stages[stage][2]);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + stage * 4, stages[stage][3]);
    blk_end(p);
  }
  // Stage 4: spare0.rgb = spare1.rgb * spare0.a; its alpha part discards.
  p = blk_begin();
  p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 4 * 4,
               MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, kSpare1) |
                   MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, kSpare0) |
                   MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 1));
  p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 4 * 4, MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, kSpare0));
  p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 4 * 4, 0);
  p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 4 * 4, 0);
  p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
               MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 5) |
                   MASK(NV097_SET_COMBINER_CONTROL_MUX_SELECT, NV097_SET_COMBINER_CONTROL_MUX_SELECT_MSB));
  // Final combiner: A * B + (1 - A) * C + D with A = B = C = 0, D = spare0;
  // alpha (G) = spare0's.
  p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, kSpare0));
  p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,
               MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, kSpare0) |
                   MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1));
  blk_end(p);
  g_combiner = kGridLine;
}

// The glowing lines' pixel shaders (ED8C5D26, 30398AC8 the lines, CABF22DB
// their glow outlines):
//   colour = C * (1 - |u|)^4             (the lines)
//   colour = C * (1 - |u|)^4 * a^6       (the outlines: a runs from 1 at the
//                                         line to 0 at the outline's edge)
// with C brighter than 1 (the sparks are 18 times white): on the 360 the
// line stays at full brightness over half its width before fading. The
// NV2A clamps vertex colours to 1, so the vertex program sends C / 16
// (kStrokeConstant1) and the combiners scale back up before clamping:
//   stage 0: spare1.rgb = 4 * t0 * diffuse  (t0 the curve, g_falloff)    spare0.a = a * a
//   stage 1: spare1.rgb = 4 * spare1         = min(1, C * curve)          spare1.a = spare0.a ^ 2 = a^4
//   stage 2:                                                              spare0.a = spare1.a * spare0.a = a^6
//                                                                         (1 for the lines)
//   stage 3: spare0.rgb = spare1.rgb * spare0.a
// The lines' 30398AC8 also replaces red with a function of alpha; left out.
const float kStrokeConstant1[4] = {1.0f / 16, 1.0f / 16, 1.0f / 16, 1};

void use_stroke_combiner(bool glow) {
  const Combiner want = glow ? kStrokeGlow : kStrokeCore;
  if (g_combiner == want) return;
  constexpr u32 kZero = 0, kDiffuse = 4, kTexture0 = 8, kSpare0 = 0xC, kSpare1 = 0xD, kInvert = 1;
  auto alpha_in = [](u32 a, u32 a_map, u32 b, u32 b_map) {
    return MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, a) | MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) |
           MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, a_map) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, b) |
           MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, b_map);
  };
  auto alpha_out = [](u32 ab) { return MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, ab); };
  auto colour_in = [](u32 a, u32 a_map, u32 b, u32 b_map, bool b_alpha) {
    return MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, a) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, a_map) |
           MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, b) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, b_map) |
           MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, b_alpha ? 1 : 0);
  };
  const u32 times4_spare1 = MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, kSpare1) |
                            MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY2);
  const u32 stages[4][4] = {
      {colour_in(kTexture0, 0, kDiffuse, 0, false), times4_spare1, alpha_in(kDiffuse, 0, kDiffuse, 0),
       alpha_out(kSpare0)},
      {colour_in(kSpare1, 0, kZero, kInvert, false), times4_spare1, alpha_in(kSpare0, 0, kSpare0, 0),
       alpha_out(kSpare1)},
      // a^6 for the outlines; 1 (zero inverted) for the lines
      {0, 0, glow ? alpha_in(kSpare1, 0, kSpare0, 0) : alpha_in(kZero, kInvert, kZero, kInvert), alpha_out(kSpare0)},
      {colour_in(kSpare1, 0, kSpare0, 0, true), MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, kSpare0), 0, 0},
  };
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
  p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
               MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE));
  blk_end(p);
  for (u32 stage = 0; stage < 4; ++stage) {
    p = blk_begin();
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + stage * 4, stages[stage][0]);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + stage * 4, stages[stage][1]);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + stage * 4, stages[stage][2]);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + stage * 4, stages[stage][3]);
    blk_end(p);
  }
  p = blk_begin();
  p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
               MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 4) |
                   MASK(NV097_SET_COMBINER_CONTROL_MUX_SELECT, NV097_SET_COMBINER_CONTROL_MUX_SELECT_MSB));
  p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, kSpare0));
  p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,
               MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, kSpare0) |
                   MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1));
  blk_end(p);
  g_combiner = want;
}

// One screen-aligned quad from (x0, y0) to (x1, y1); unit k's texture
// coordinates run from uv[k][0..1] to uv[k][2..3] across it.
void glow_quad(float x0, float y0, float x1, float y1, const float uv[4][4]) {
  constexpr u32 kStride = 40;  // x, y, then u, v for each of the four units
  if (!arena_room(4 * kStride)) return;
  float* v = (float*)(g_arena + g_arena_used);
  const float corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int c = 0; c < 4; ++c) {
    float* out = v + c * 10;
    out[0] = corner[c][0] ? x1 : x0;
    out[1] = corner[c][1] ? y1 : y0;
    for (int k = 0; k < 4; ++k) {
      out[2 + k * 2] = corner[c][0] ? uv[k][2] : uv[k][0];
      out[3 + k * 2] = corner[c][1] ? uv[k][3] : uv[k][1];
    }
  }
  u32 formats[16] = {}, offsets[16] = {};
  const u32 base = ((u32)g_arena & 0x03FFFFFF) + g_arena_used;
  formats[0] = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F | (2 << 4) | (kStride << 8);
  offsets[0] = base;
  for (int k = 0; k < 4; ++k) {
    formats[9 + k] = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F | (2 << 4) | (kStride << 8);
    offsets[9 + k] = base + 8 + k * 8;
  }
  set_arrays(formats, offsets);
  g_arena_used += (4 * kStride + 15) & ~15u;
  draw_arrays(NV097_SET_BEGIN_END_OP_QUADS, 4);
}

// A pass sampling 'from' (all four units) at four offsets in texels, into
// the rectangle (0, 0)-(w, h) of the current target; texel = scale * screen
// pixel + base, so a texture coordinate runs from base to base + scale * w.
void glow_pass(const void* from, u32 from_w, u32 from_h, u32 pitch, float w, float h, float scale,
               const float offsets[4][2], const float weights[4], float dest_y = 0) {
  for (u32 unit = 0; unit < 4; ++unit) bind_linear(unit, from, from_w, from_h, pitch);
  set_sum_weights(weights);
  float uv[4][4];
  for (int k = 0; k < 4; ++k) {
    uv[k][0] = offsets[k][0];
    uv[k][1] = offsets[k][1];
    uv[k][2] = offsets[k][0] + scale * w;
    uv[k][3] = offsets[k][1] + scale * h;
  }
  glow_quad(0, dest_y, w, dest_y + h, uv);
}

// --- The grid's colour field ----------------------------------------------------------------
// The game colours its grid from a 150x100 field of light it keeps on the GPU
// (from a level capture on hardware): each frame
//   1. every light-giving object splats a soft blob (vertex shader C5C713E2,
//      a 31x31 8-bit spot times the object's colour, brighter than 1: the
//      big ambient lights are colours like (5, 2.6, 21)), added up;
//   2. the splats are shrunk to 75x50, 37x25, 18x12, 9x6, 4x3 and 2x1;
//   3. field = 0.75 * (0.25 splats + 0.1875 * each smaller level, enlarged)
//      + 0.5 * last frame's field (moved along with the grid's distortion);
// and the grid lines read it (use_line_combiner). The widest levels spread
// an average tint over the whole arena: the grid's general colour.
//
// Here at half size (75x50, in the corners of pbkit's extra buffers, 16-bit,
// divided by kFieldScale to fit), without the distortion's movement. Each
// level is one bilinear shrink of the one before, then (levels 1-4) blurred
// across and down with four bilinear taps, eight texels wide (the game uses
// 15-tap blurs):
//   extra 0: splats (0,0) 75x50, level 2 at (100,0), level 4 at (130,0),
//            blur scratch for levels 1 and 3 at (0,64) and (100,64)
//   extra 1: level 1 at (0,0), level 3 at (100,0), level 5 at (140,0),
//            last frame's field at (0,64), blur scratch for levels 2 and 4
//            at (100,64) and (130,64)
//   extra 2: the field at (0,0), the step-3 part sum at (0,64)
// Each pass reads a region another pass finished (WAIT_FOR_IDLE between).
constexpr float kFieldScale = 4;
constexpr u32 kFieldW = 75, kFieldH = 50;
const float kSplatConstant1[4] = {1 / kFieldScale, 1 / kFieldScale, 1 / kFieldScale, 1};
bool g_field_open = false;   // this frame's splats are being drawn
bool g_field_ready = false;  // extra buffer 2 holds a field

struct FieldTap {
  int buffer;            // extra buffer
  float x0, y0, x1, y1;  // its region, read across the destination
  float weight;
};

// One pass into extra buffer 'target', rectangle (x0, y0)-(x1, y1), the sum
// of up to four taps (bilinear).
void field_pass(int target, float x0, float y0, float x1, float y1, const FieldTap* taps, int n) {
  const u32 width = pb_back_buffer_width(), height = pb_back_buffer_height(), pitch = pb_back_buffer_pitch();
  const WorkArea& to = g_work[2 + target];
  glow_target(to.buffer);
  y0 += (float)to.row;
  y1 += (float)to.row;
  float uv[4][4] = {}, weights[4] = {};
  for (int k = 0; k < 4; ++k) {
    const FieldTap& t = taps[k < n ? k : 0];
    bind_linear((u32)k, work_pixels(2 + t.buffer), width, height - g_work[2 + t.buffer].row, pitch);
    uv[k][0] = t.x0;
    uv[k][1] = t.y0;
    uv[k][2] = t.x1;
    uv[k][3] = t.y1;
    weights[k] = k < n ? t.weight : 0;
  }
  set_sum_weights(weights);
  glow_quad(x0, y0, x1, y1, uv);
}

void field_pass_state(bool on) {
  if (on) {
    set_program_start(kGlowProgramSlot);
    use_sum_combiner();
    g_combiner = kOther;
  }
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_BLEND_ENABLE, on ? 0 : 1);
  blk_end(p);
}

// Before the frame's first splat: last frame's field put aside, the work
// regions cleared, the splats' target set.
void field_begin() {
  if (g_field_open) return;
  field_pass_state(true);
  // Region k of the field's work areas cleared (160x116).
  auto clear = [](int k) {
    glow_target(g_work[2 + k].buffer);
    pb_fill(0, (int)g_work[2 + k].row, 160, 116, 0);
  };
  if (!g_field_ready) {
    clear(2);
    g_field_ready = true;
  }
  clear(1);
  const FieldTap last = {2, 0, 0, (float)kFieldW, (float)kFieldH, 1};
  field_pass(1, 0, 64, kFieldW, 64 + kFieldH, &last, 1);
  clear(0);
  glow_target(g_work[2].buffer);  // the splats' target
  field_pass_state(false);
  // The splats: viewport 150x100 in the game's units, drawn at half that.
  g_target_scale_x = (float)kFieldW / 150;
  g_target_scale_y = (float)kFieldH / 100;
  g_target_row = (float)g_work[2].row;
  g_field_open = true;
}

// Blurs the w x h level at (x, y) of extra buffer 'buffer' across into the
// scratch at (x, y + 64) of the other buffer, and down back into place.
void field_blur(int buffer, float x, float y, float w, float h) {
  const int other = 1 - buffer;
  const float sy = y + 64;
  FieldTap across[4], down[4];
  const float offsets[4] = {-2.5f, -0.5f, 1.5f, 3.5f};  // texel pairs (-3,-2) ... (3,4)
  for (int k = 0; k < 4; ++k) {
    across[k] = {buffer, x + offsets[k], y, x + w + offsets[k], y + h, 0.25f};
    down[k] = {other, x, sy - offsets[k], x + w, sy + h - offsets[k], 0.25f};
  }
  field_pass(other, x, sy, x + w, sy + h, across, 4);
  field_pass(buffer, x, y, x + w, y + h, down, 4);
}

// After the last splat: the levels and the field, then back to the screen.
void field_end() {
  if (!g_field_open) return;
  g_field_open = false;
  g_target_scale_x = g_scale_x;
  g_target_scale_y = g_scale_y;
  g_target_row = g_screen_row;
  field_pass_state(true);
  // Each level a bilinear sample between 2x2 texels of the one before.
  const FieldTap l1 = {0, 0, 0, 74, 50, 1};
  field_pass(1, 0, 0, 37, 25, &l1, 1);
  field_blur(1, 0, 0, 37, 25);
  const FieldTap l2 = {1, 0, 0, 36, 24, 1};
  field_pass(0, 100, 0, 118, 12, &l2, 1);
  field_blur(0, 100, 0, 18, 12);
  const FieldTap l3 = {0, 100, 0, 118, 12, 1};
  field_pass(1, 100, 0, 109, 6, &l3, 1);
  field_blur(1, 100, 0, 9, 6);
  const FieldTap l4 = {1, 100, 0, 108, 6, 1};
  field_pass(0, 130, 0, 134, 3, &l4, 1);
  field_blur(0, 130, 0, 4, 3);
  const FieldTap l5 = {0, 130, 0, 134, 2, 1};
  field_pass(1, 140, 0, 142, 1, &l5, 1);
  const float w = (float)kFieldW, h = (float)kFieldH;
  // 0.25 splats + 0.1875 (levels 1-3), then 0.75 of that + 0.75 * 0.1875
  // (levels 4, 5) + 0.5 last frame's field.
  const FieldTap part[4] = {{0, 0, 0, w, h, 0.25f},
                            {1, 0, 0, 37, 25, 0.1875f},
                            {0, 100, 0, 118, 12, 0.1875f},
                            {1, 100, 0, 109, 6, 0.1875f}};
  field_pass(2, 0, 64, w, 64 + h, part, 4);
  const FieldTap field[4] = {{2, 0, 64, w, 64 + h, 0.75f},
                             {0, 130, 0, 134, 3, 0.140625f},
                             {1, 140, 0, 142, 1, 0.140625f},
                             {1, 0, 64, w, 64 + h, 0.5f}};
  field_pass(2, 0, 0, w, h, field, 4);
  glow_target(-1);
  field_pass_state(false);
}

void apply_glow() {
  const u32 width = pb_back_buffer_width(), height = pb_back_buffer_height(), pitch = pb_back_buffer_pitch();
  // The small buffers hold a (width / 4) x (height / 4) picture: sampled as
  // that size, so the blur clamps at its edge instead of reading the stale
  // rest of the buffer (coloured specks along the bottom on hardware).
  const u32 sw = width / 4, sh = height / 4;
  const float w = (float)sw, h = (float)sh;
  const void* picture = pb_back_buffer();
  const void* small = work_pixels(0);
  const void* blurred = work_pixels(1);
  const float small_y = (float)g_work[0].row, blurred_y = (float)g_work[1].row;
  set_program_start(kGlowProgramSlot);
  use_sum_combiner();
  g_combiner = kOther;
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
  blk_end(p);

  // 1. A quarter of the size: four bilinear taps, each the average of 2x2,
  // cover the 4x4 block (output pixel x reads around source 4x + 1.5).
  glow_target(g_work[0].buffer);
  const float quarter[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  const float even[4] = {0.25f, 0.25f, 0.25f, 0.25f};
  glow_pass(picture, width, height, pitch, w, h, 4, quarter, even, small_y);
  // 2, 3. Blur across, then down: four bilinear taps, eight texels.
  glow_target(g_work[1].buffer);
  const float across[4][2] = {{-2.5f, 0}, {-0.5f, 0}, {1.5f, 0}, {3.5f, 0}};
  glow_pass(small, sw, sh, pitch, w, h, 1, across, even, blurred_y);
  glow_target(g_work[0].buffer);
  const float down[4][2] = {{0, -2.5f}, {0, -0.5f}, {0, 1.5f}, {0, 3.5f}};
  glow_pass(blurred, sw, sh, pitch, w, h, 1, down, even, small_y);

  // 4. Added back over the picture, enlarged (bilinear).
  glow_target(-1);
  p = blk_begin();
  p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
  p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, 0x0001);  // one
  p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, 0x0001);  // one
  p = pb_push1(p, NV097_SET_BLEND_EQUATION, 0x8006);      // add
  blk_end(p);
  const float none[4][2] = {};
  const float strength[4] = {kGlowStrength, 0, 0, 0};
  glow_pass(small, sw, sh, pitch, (float)width, (float)height, 0.25f, none, strength);

  use_default_combiner();
}

}  // namespace

#ifdef GW_SPAWN_CAPTURE
void capture_trigger();  // below gw_nv2a_swap
#endif

void gw_nv2a_draw(u32 opcode, const u32* words, u32 n, const u32* regs, u32 vs_hash, const XvsProgram* vs,
                  u32 ps_hash) {
  if (!g_ready) return;
  u32 initiator = opcode == 0x22 ? words[1] : words[0];
  u32 prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
  g_census_entry = nullptr;
  for (CensusEntry& e : g_census) {
    if (!e.hash || (e.hash == vs_hash && e.ps == ps_hash)) {
      e.hash = vs_hash;
      e.ps = ps_hash;
      ++e.draws;
      e.prim = prim;
      e.vertices += count;
      g_census_entry = &e;
      break;
    }
  }
  if (!vs || count == 0) return;
  // Colour draws into the main 1280-wide scene, and the light splats of the
  // grid's colour field (its 160-pitch target, vertex shader C5C713E2). The
  // rest of the field (its moving with the distortion), the glow chain,
  // resolves (mode 6) and clears (mode 5) are not reproduced.
  const u32 mode = regs[0x2208] & 7, pitch = regs[0x2000] & 0x3FFF;
  const bool field_splat = mode == 4 && pitch == 160 && vs_hash == 0xC5C713E2;
  if (!field_splat && (mode != 4 || pitch != 1280)) {
    // detail: mode << 16 | surface pitch
    return note_skip(kNotMainTarget, mode << 16 | pitch);
  }
  u32 nv_prim = prim == 1 ? NV097_SET_BEGIN_END_OP_POINTS
              : prim == 2 ? NV097_SET_BEGIN_END_OP_LINES
              : prim == 3 ? NV097_SET_BEGIN_END_OP_LINE_STRIP
              : prim == 4 ? NV097_SET_BEGIN_END_OP_TRIANGLES
              : prim == 5 ? NV097_SET_BEGIN_END_OP_TRIANGLE_FAN
              : prim == 6 ? NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP
              : prim == 13 ? NV097_SET_BEGIN_END_OP_QUADS
              : 0;
  if (!nv_prim) return note_skip(kPrimitive, prim);
  if ((prim == 3 || prim == 5 || prim == 6) && count > 252) return;  // strips/fans aren't split
  // Index source: 0 = index buffer (DRAW_INDX: viz, initiator, base, size),
  // 2 = auto (0..count-1).
  u32 index_base = 0, index_endian = 0;
  bool index32 = (initiator >> 11) & 1;
  if (source == 0) {
    if (opcode != 0x22 || n < 4) return;
    index_base = gw_gpu_to_guest(words[2]);
    index_endian = words[3] >> 30;
  } else if (source != 2) {
    return;
  }
  if (!g_in_frame) begin_frame();
  if (field_splat) {
    field_begin();
  } else {
    field_end();
  }

  float vx_scale = as_float(regs[0x210F]), vx_off = as_float(regs[0x2110]);
  float vy_scale = as_float(regs[0x2111]), vy_off = as_float(regs[0x2112]);
  u32 vte = regs[0x2206];

  // The in-level grid's lines (vertex shader 8E1A294A, pixel shader
  // B16AAC17): their own program and combiner (see use_line_combiner).
  const bool grid_lines = vs_hash == 0x8E1A294A;
  // Pixel shader: which interpolator is the texture coordinate.
  int uv_i = -1, tex_const = 0;
  xps_info(ps_hash, &uv_i, &tex_const);
  const Texture* texture = &g_white;
  // The glowing lines and the vector font: u (interpolator 1) through the
  // falloff curve (see g_falloff).
  const bool stroke =
      (ps_hash == 0xED8C5D26 || ps_hash == 0xCABF22DB || ps_hash == 0x30398AC8) && g_falloff.pixels;
  if (stroke) {
    uv_i = 1;
    texture = &g_falloff;
    if (g_census_entry) ++g_census_entry->strokes;
  } else if (prim != 1 && !grid_lines) {
    // A draw whose texture can't be reproduced yet (render targets, float
    // formats, computed coordinates) is skipped: many use replace blending
    // and would paint flat shapes over the scene.
    if (uv_i == -2) return note_skip(kComputedUv, 0);
    if (uv_i >= 0) {
      const Texture* t = texture_from_fetch(&regs[0x4800 + tex_const * 6]);
      const u32 texture_format = regs[0x4800 + tex_const * 6 + 1] & 0x3F;
      // The arena's border (vertex shader ADB9F3EC: four sides, two quads
      // each) reads the colour field's 18x12 level, a 16_16_16_16 render
      // target not reproduced here: drawn with white, its vertex colours
      // giving its look (bright on the 360 whatever the field).
      if (!t && vs_hash == 0xADB9F3EC) t = &g_white;
      if (!t) return note_skip(kTextureFormat, texture_format);
      texture = t;
    }
  }
  if (uv_i < 0 || grid_lines) uv_i = -1;
  int color_i = grid_lines ? -1 : uv_i == 0 ? 1 : 0;
  if (grid_lines) {
    bind_linear(0, work_pixels(4), kFieldW, kFieldH, pb_back_buffer_pitch());  // the colour field
    use_line_combiner();
  } else if (stroke) {
    bind_texture(texture);
    use_stroke_combiner(ps_hash == 0xCABF22DB);
  } else {
    bind_texture(texture);
    use_default_combiner();
  }

  u32 blend = regs[0x2201];
  u32* p = blk_begin();
  p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, blend_factor(blend & 0x1F));
  p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, blend_factor((blend >> 8) & 0x1F));
  // Xenos BlendOp (add, subtract, min, max, reverse subtract) -> GL equation.
  static const u32 kEquations[5] = {0x8006, 0x800A, 0x8007, 0x8008, 0x800B};
  u32 op = (blend >> 5) & 7;
  p = pb_push1(p, NV097_SET_BLEND_EQUATION, op < 5 ? kEquations[op] : 0x8006);
  if (prim == 1) p = pb_push1(p, NV097_SET_POINT_SIZE, 16);  // 2 px (units of 1/8)
  blk_end(p);

  // Preferred: the shader as an NV2A vertex program. Draws larger than half
  // the vertex arena go in pieces (whole primitives: a multiple of 12 is
  // whole quads, triangles, lines and rectangles); the grid's lines are a
  // 65528-vertex draw.
  if (g_use_gpu_vertex) {
    const XvpProgram* gp = xvp_get(vs, uv_i, color_i, !(vte & 0x100), grid_lines ? 0 : -1, field_splat || stroke);
    // The particles' splats are only in the glow buffers' mirrors
    // (src/particle_native.cpp): a splat draw reaching past its mirror draws
    // the mirrored part.
    if (gp && field_splat && source == 2 && gp->stride) {
      const u32 stream = gw_gpu_to_guest(regs[0x4800 + gp->fetch_slot * 2] & ~3u);
      const u32 start = stream + regs[0x2102] * gp->stride;
      const u32 room = gw_vertex_mirror_room(start);  // bytes from start to its mirror's end
      if (room && room < count * gp->stride) count = room / gp->stride / 4 * 4;
    }
    if (gp && count) {
      const u32 piece = gp->stride ? (kArenaBytes / 2 / gp->stride) / 12 * 12 : count;
      bool drawn = true;
      for (u32 first = 0; first < count && drawn; first += piece) {
        const u32 n = count - first < piece ? count - first : piece;
        drawn = draw_on_gpu(gp, regs, nv_prim, first, n, source, index_base, index_endian, index32, vte, false,
                            grid_lines ? (float)kFieldW : texture->u_scale, grid_lines ? (float)kFieldH : texture->v_scale,
                            grid_lines ? kLineConstant1 : nullptr,
                            field_splat ? kSplatConstant1 : stroke ? kStrokeConstant1 : nullptr);
      }
      if (drawn) {
        if (g_census_entry) {
          g_census_entry->gpu += count;
          ++g_census_entry->drawn;
        }
        note_order(vs_hash, ps_hash);
        return;
      }
    }
  }
  if (grid_lines || field_splat) return;  // no CPU path for these

  // Fallback: interpret the shader on the CPU, draw pre-transformed vertices.
  if (!arena_room(count * 20)) {
    static u32 reported = 0;
    if (reported++ < 4) {
      gw_log("nv2a: frame %u draw of %u vertices is larger than the vertex arena (shader %08X)\n", g_frames, count,
             vs_hash);
    }
    return;
  }
  if (g_census_entry) {
    g_census_entry->cpu += count;
    ++g_census_entry->drawn;
  }
  note_order(vs_hash, ps_hash);
  set_program_start(0);
  u32* out = (u32*)(g_arena + g_arena_used);
  for (u32 i = 0; i < count; ++i) {
    u32 index = i;
    if (source == 0) {
      u32 raw;
      if (index32) {
        memcpy(&raw, gw_host(index_base + i * 4), 4);
        index = swap_endian(raw, index_endian);
      } else {
        u16 h;
        memcpy(&h, gw_host(index_base + i * 2), 2);
        raw = h;
        index = index_endian ? (u16)((raw << 8) | (raw >> 8)) : raw;
      }
    }
    float clip[4], interp[8][4];
    xvs_run(vs, regs, index + regs[0x2102], clip, interp);  // with VGT_INDX_OFFSET
    // PA_CL_VTE_CNTL: bit 8 = xy already divided, bits 0-3 = viewport
    // x/y scale and offset enables (off when the game sends pixels).
    float x = clip[0], y = clip[1];
    if (!(vte & 0x100) && clip[3] != 0.0f) {
      x /= clip[3];
      y /= clip[3];
    }
    if (vte & 1) x *= vx_scale;
    if (vte & 2) x += vx_off;
    if (vte & 4) y *= vy_scale;
    if (vte & 8) y += vy_off;
    float sx = x * g_scale_x, sy = y * g_scale_y + g_screen_row;
    float c[4] = {interp[color_i][0], interp[color_i][1], interp[color_i][2], interp[color_i][3]};
    if (stroke)
      for (int k = 0; k < 3; ++k) c[k] *= kStrokeConstant1[k];
    float u = uv_i >= 0 ? interp[uv_i][0] * texture->u_scale : 0, t = uv_i >= 0 ? interp[uv_i][1] * texture->v_scale : 0;
    // Packed vertex: x, y, D3DCOLOR, u, v.
    memcpy(&out[0], &sx, 4);
    memcpy(&out[1], &sy, 4);
    out[2] = pack_color(c);
    memcpy(&out[3], &u, 4);
    memcpy(&out[4], &t, 4);
    out += 5;
  }
  u32 base = ((u32)g_arena & 0x03FFFFFF) + g_arena_used;
  u32 formats[16] = {}, offsets[16] = {};
  formats[0] = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F | (2 << 4) | (20 << 8);
  formats[3] = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D | (4 << 4) | (20 << 8);
  formats[9] = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F | (2 << 4) | (20 << 8);
  offsets[0] = base;
  offsets[3] = base + 8;
  offsets[9] = base + 12;
  set_arrays(formats, offsets);
  g_arena_used += count * 20;
  if (g_census_entry) g_census_entry->arena_bytes += count * 20;
  draw_arrays(nv_prim, count);
}

// Called on each guest swap: shows the frame drawn since the last one.
#ifdef GW_SPAWN_CAPTURE
// Frames around the ship's spawn, for tools/xbdm_frames.py: every drawn
// frame, shrunk to a quarter (320x180, R5G6B5, one pixel of each 4x4), into
// a ring of kCaptureFrames; the spawn (a particle draw past 2048 vertices
// after none for 30 frames, see the audit in gw_nv2a_draw) freezes the ring
// kCaptureFrames / 2 frames later, so it holds the frames before and after.
// gw_capture_state: [0] 'CAPT', [1] frames written (ring position = [1] %
// kCaptureFrames), [2] 1 once frozen, [3] frames left before freezing
// (0 = not triggered).
constexpr u32 kCaptureFrames = 24, kCaptureW = 320, kCaptureH = 180;
extern "C" {
volatile u32 gw_capture_state[4] = {0x43415054, 0, 0, 0};
u16 gw_capture_frames[kCaptureFrames][kCaptureH][kCaptureW];
}
void capture_trigger() {
  // The level start's spawn draws correctly; the freeze is by white area
  // (capture_frame) instead.
}
void capture_frame() {
  if (gw_capture_state[2]) return;
  while (pb_busy()) {
  }
  const u8* picture = (const u8*)pb_back_buffer();
  const u32 pitch = pb_back_buffer_pitch(), w = pb_back_buffer_width(), h = pb_back_buffer_height();
  u16(*out)[kCaptureW] = gw_capture_frames[gw_capture_state[1] % kCaptureFrames];
  u32 white = 0;  // near-white pixels (the stray polygons are large and white)
  for (u32 y = 0; y < kCaptureH; ++y) {
    const u16* row = (const u16*)(picture + (y * h / kCaptureH) * pitch);
    for (u32 x = 0; x < kCaptureW; ++x) {
      const u16 px = row[x * w / kCaptureW];
      out[y][x] = px;
      white += (px >> 11) >= 28 && ((px >> 5) & 63) >= 56 && (px & 31) >= 28;
    }
  }
  gw_capture_state[1] = gw_capture_state[1] + 1;
  // A quarter of the frame near-white: freeze a few frames later.
  if (white > kCaptureW * kCaptureH / 4 && !gw_capture_state[3]) gw_capture_state[3] = 6;
  if (gw_capture_state[3] && --gw_capture_state[3] == 0) gw_capture_state[2] = 1;
}
#endif

void gw_nv2a_swap() {
  if (!g_ready) return;
  // Nothing drawn since the last swap (a frame the game updated but did not
  // render, src/render_native.cpp): the frame on screen stays.
  if (!g_in_frame) return;
  ++g_frames;
  // Converted textures not drawn with for 10 s (300 drawn frames) give their
  // memory back: the menus' large images (title art, a 1280x720 one, the
  // fonts) were held all through play, and with them the console had well
  // under 1 MB to spare in a level. One coming back is converted again. The
  // GPU finished with it long ago.
  if (g_frames % 30 == 0) {
    for (Texture& t : g_textures) {
      if (!t.pixels || t.source_released || g_frames - t.last_used < 300) continue;
      gw_log("nv2a: texture %08X unused for 10 s, %u KiB given back\n", t.key, t.bytes / 1024);
      MmFreeContiguousMemory(t.pixels);
      t.pixels = nullptr;
    }
  }
  ++gw_stats.drawn;
  if (g_frames % 300 == 0) {
    gw_log("nv2a: frame %u vertex shaders:\n", g_frames);
    for (const CensusEntry& e : g_census) {
      if (e.hash) {
        gw_log("  %08X ps %08X: %u draws (%u as strokes), prim %u, %u vertices (%u on the GPU, %u on the CPU; "
               "%u in place, arena %u KiB)%s%s %X\n",
               e.hash, e.ps, e.draws, e.strokes, e.prim, e.vertices, e.gpu, e.cpu, e.in_place, e.arena_bytes / 1024,
               e.skip ? "; skipped: " : "", kSkipNames[e.skip], e.skip_detail);
      }
    }
  }
  // Flicker detector: a drawn frame with far fewer draws reaching the NV2A
  // than the one before (the menus lost their foreground for single frames)
  // is logged with what changed: each kind of draw's count now and before,
  // why any were skipped, and this frame's texture conversions (and
  // failures) and vertex arena refills.
  // A frame missing most of the last shown frame's draws is not shown: at a
  // screen change the menus draw about a quarter of a second of frames
  // without their foreground (seen on the console: 7 drawn frames), a
  // visible flicker. The last frame stays up instead, for up to kMaxHeld
  // frames; a drop that lasts longer is a real change and is shown. (Drawing
  // every update meanwhile made the game's gap longer: it is not done.)
  bool skip_frame = false;
  {
    // Up to kMaxHeld frames in a row; a drop that lasts longer is a real
    // change of screen and is shown.
    constexpr u32 kMaxHeld = 12;
    static u32 held = 0, low_run = 0;
    u32 now = 0, before = 0;
    for (const CensusEntry& e : g_census) now += e.drawn;
    for (const CensusEntry& e : g_census_last) before += e.drawn;
    const bool low = before >= 12 && now * 2 < before;
    skip_frame = low && held < kMaxHeld;
    held = skip_frame ? held + 1 : 0;
    static u32 told = 0, told_runs = 0;
    if (low) {
      ++low_run;
    } else if (low_run) {
      if (told_runs++ < 20) gw_log("nv2a: frame %u: the foreground back after %u frames without\n", g_frames, low_run);
      low_run = 0;
    }
    if (skip_frame && told < 20) {
      ++told;
      gw_log("nv2a: frame %u not shown (%u draws, the last one shown %u)\n",
             g_frames, now, before);
    }
    if (before >= 12 && now * 10 < before * 7 && told < 40) {
      ++told;
      gw_log("nv2a: frame %u drew %u draws, the one before %u (textures converted %u, failed %u; arena refills %u)\n",
             g_frames, now, before, g_frame_conversions, g_frame_conversion_failures, g_frame_arena_refills);
      for (const CensusEntry& e : g_census_last) {
        if (!e.hash || !e.drawn) continue;
        const CensusEntry* c = nullptr;
        for (const CensusEntry& n : g_census)
          if (n.hash == e.hash && n.ps == e.ps) c = &n;
        const u32 drawn_now = c ? c->drawn : 0;
        if (drawn_now >= e.drawn) continue;
        gw_log("  %08X ps %08X: drawn %u -> %u (issued now %u)%s%s %X\n", e.hash, e.ps, e.drawn, drawn_now,
               c ? c->draws : 0, c && c->skip ? "; skipped: " : "", c ? kSkipNames[c->skip] : "",
               c ? c->skip_detail : 0);
      }
    }
  }
  // Order check: the kinds both frames drew, in each frame's order; logged
  // when the sequence differs (a black panel moving after the text, say).
  {
    u32 a[kOrderMax], b[kOrderMax], na = 0, nb = 0;
    auto in = [](u32 (*list)[2], u32 n, u32 vs, u32 ps) {
      for (u32 i = 0; i < n; ++i)
        if (list[i][0] == vs && list[i][1] == ps) return true;
      return false;
    };
    for (u32 i = 0; i < g_order_count; ++i)
      if (in(g_order_last, g_order_last_count, g_order[i][0], g_order[i][1])) a[na++] = i;
    for (u32 i = 0; i < g_order_last_count; ++i)
      if (in(g_order, g_order_count, g_order_last[i][0], g_order_last[i][1])) b[nb++] = i;
    bool moved = false;
    for (u32 k = 0; k < na && k < nb; ++k)
      moved |= g_order[a[k]][0] != g_order_last[b[k]][0] || g_order[a[k]][1] != g_order_last[b[k]][1];
    static u32 told = 0;
    if (moved && told < 20) {
      ++told;
      gw_log("nv2a: frame %u drew its kinds of draw in another order than the frame before:\n", g_frames);
      for (u32 i = 0; i < g_order_last_count; ++i) gw_log("  before %u: %08X ps %08X\n", i, g_order_last[i][0], g_order_last[i][1]);
      for (u32 i = 0; i < g_order_count; ++i) gw_log("  now    %u: %08X ps %08X\n", i, g_order[i][0], g_order[i][1]);
    }
    memcpy(g_order_last, g_order, sizeof(g_order));
    g_order_last_count = g_order_count;
    g_order_count = 0;
  }
  if (!skip_frame) memcpy(g_census_last, g_census, sizeof(g_census));
  g_frame_conversions = g_frame_conversion_failures = g_frame_arena_refills = 0;
  memset(g_census, 0, sizeof(g_census));
  if (gw_census_due()) {
    gw_log_memory("census");
    gw_dump_allocations();
  }
  field_end();
  if (kGlow) apply_glow();
  blk_flush();
#ifdef GW_SPAWN_CAPTURE
  capture_frame();
#endif
#ifdef GW_OVERLAY
  pb_erase_text_screen();
  pb_print("Geometry Wars RE2 - Xbox  frame %u\n", g_frames);
  pb_draw_text_screen();
#endif
  // The GPU draws this frame while the game builds the next one: the next
  // frame's pb_reset (begin_frame) is what waits for it, so the vertex arena
  // is only reused once the GPU is done with it. Here the flip is queued,
  // which only waits while all three buffers are still on their way to the
  // screen. Bounded: a stuck NV2A is reported instead of freezing the game.
  if (skip_frame) {
    gw_nv2a_stage = 0;
    g_in_frame = false;
    return;
  }
  gw_nv2a_stage = 4;
  const u64 queued = gw_wait_begin();
  DWORD start = GetTickCount();
  // Sleeping, not spinning, while the flip queue is full: this thread is
  // above the game's, so a spin here took the one core from the game.
  while (pb_finished()) {
    if (GetTickCount() - start > 1000) {
      gw_log("nv2a: swap not accepted after 1 s at frame %u\n", g_frames);
      break;
    }
    Sleep(1);
  }
  gw_wait_end(queued, 0, "renderer: flip queue full");
  gw_nv2a_stage = 0;
  g_in_frame = false;
}
