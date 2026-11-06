// NV2A renderer (pbkit), first stage: the title screen's scene layers.
//
// The game's draws are recognised by a fingerprint of their vertex shader.
// For the few simple ones (position through a 4x4 matrix, optional texture
// coordinates and colour) the vertices are read from guest memory and
// transformed on the CPU, then drawn with a pass-through vertex program and a
// texture-times-colour combiner. Everything else (the glow chain, resolves,
// clears, the grid) is skipped for now.
#include "runtime.h"

#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#ifndef MASK
#define MASK(mask, value) (((value) << (__builtin_ffs(mask) - 1)) & (mask))
#endif

namespace {

// Vertex shader fingerprints (FNV-1a of the microcode dwords), from
// xbox/tools/gpu_capture.py captures.
constexpr u32 kVsStars = 0xA1E03FA2;   // points: float3 pos, float3 colour (stride 24)
constexpr u32 kVsQuads = 0xADB9F3EC;   // quads: float3 pos, float4 colour (stride 28)
constexpr u32 kVsImage = 0x9DD94822;   // quads: float2 pos, float2 uv, float3 colour (stride 28)

struct Layout {
  u32 hash;
  u32 stride;
  u32 pos_count;    // floats at offset 0
  int uv_offset;    // -1: none
  int color_offset;
  u32 color_count;
};

const Layout kLayouts[] = {
    {kVsStars, 24, 3, -1, 12, 3},
    {kVsQuads, 28, 3, -1, 12, 4},
    {kVsImage, 28, 2, 8, 16, 3},
};

bool g_ready = false;
}  // namespace

volatile bool gw_nv2a_active = false;  // the status page stops drawing
// Where the command processor thread is in the renderer: 0 idle, 1 pb_reset,
// 2 drawing, 3 waiting for the GPU, 4 waiting for the swap.
volatile u32 gw_nv2a_stage = 0;

namespace {
bool g_in_frame = false;
constexpr u32 kFrameBudget = 448 * 1024;  // of pbkit's default 512 KiB
u32 g_frame_bytes;
float g_scale_x, g_scale_y;
u32 g_frames;

struct Texture {
  u32 key;  // gpu address ^ size ^ format
  u32* pixels;
  u32 format;
};
Texture g_textures[32];
Texture g_white;

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

u32 nv_format(u32 w, u32 h) {
  return 1 | (2 << 4) | NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE |
         (NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8 << 8) | (1 << 16) | (log2_ceil(w) << 20) |
         (log2_ceil(h) << 24);
}

// Converts a k_8_8_8_8 texture to a swizzled A8R8G8B8 NV2A texture of at
// most 512x512 (the screen is 640x480), cached by address.
Texture* texture_from_fetch(const u32* f) {
  u32 format = f[1] & 0x3F;
  if (format != 6) return nullptr;
  u32 base = f[1] & 0xFFFFF000;
  u32 width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  u32 key = base ^ (width << 1) ^ (height << 17);
  for (Texture& t : g_textures) {
    if (t.pixels && t.key == key) return &t;
  }
  Texture* slot = nullptr;
  for (Texture& t : g_textures) {
    if (!t.pixels) {
      slot = &t;
      break;
    }
  }
  if (!slot) return nullptr;
  u32 tw = 1u << log2_ceil(width), th = 1u << log2_ceil(height);
  if (tw > 512) tw = 512;
  if (th > 512) th = 512;
  u32* pixels = (u32*)MmAllocateContiguousMemoryEx(tw * th * 4, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (!pixels) return nullptr;
  bool tiled = f[0] >> 31;
  u32 pitch = ((f[0] >> 22) & 0x1FF) * 32;
  u32 endian = (f[1] >> 6) & 3;
  const u8* src = gw_host(gw_gpu_to_guest(base));
  for (u32 y = 0; y < th; ++y) {
    u32 sy = y * height / th;
    u32 row = tiled ? tiled_row(sy, pitch, 2) : 0;
    for (u32 x = 0; x < tw; ++x) {
      u32 sx = x * width / tw;
      u32 offset = tiled ? (tiled_column(sx, sy, 2, row) >> 2) * 4 : (sy * pitch + sx) * 4;
      u32 raw;
      memcpy(&raw, src + offset, 4);
      pixels[swizzle_offset(x, y, tw, th)] = swap_endian(raw, endian);
    }
  }
  slot->key = key;
  slot->pixels = pixels;
  slot->format = nv_format(tw, th);
  gw_log("nv2a: texture %08X %ux%u%s -> %ux%u\n", base, width, height, tiled ? " tiled" : "", tw, th);
  // Large images (the title screens) are only read by the GPU: once
  // converted, the 360-format original gives its memory back. A later touch
  // recommits it as zeros.
  u32 guest = gw_gpu_to_guest(base);
  if (gw_allocation_base(guest) == guest && gw_virtual_size(guest) >= 0x40000) {
    gw_virtual_decommit(guest, gw_virtual_size(guest));
  }
  return slot;
}

float as_float(u32 bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

void begin_frame() {
  gw_nv2a_stage = 1;
  pb_reset();
  gw_nv2a_stage = 2;
  pb_target_back_buffer();
  pb_erase_depth_stencil_buffer(0, 0, pb_back_buffer_width(), pb_back_buffer_height());
  pb_fill(0, 0, pb_back_buffer_width(), pb_back_buffer_height(), 0);
  u32* p = pb_begin();
  p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
  p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
  p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
  p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
  p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
  pb_end(p);
  g_in_frame = true;
  g_frame_bytes = 0;
}

void bind_texture(const Texture* t) {
  u32* p = pb_begin();
  p = pb_push2(p, NV097_SET_TEXTURE_OFFSET, (u32)t->pixels & 0x03FFFFFF, t->format);
  p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, 0x00030303);  // clamp
  p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0x4003FFC0);
  p = pb_push1(p, NV097_SET_TEXTURE_FILTER, 0x02022000);   // linear
  pb_end(p);
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

void gw_nv2a_init() {
  u32 vs[] = {
#include "nv2a_vs.inl"
  };
  // 16-bit back buffers to match the video mode set in status.cpp (memory).
  pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false);
  if (pb_init()) {
    gw_log("nv2a: pb_init failed\n");
    return;
  }
  u32* p = pb_begin();
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
  p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
               MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                   MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
                        NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
  pb_end(p);
  for (u32 i = 0; i < sizeof(vs) / 4; i += 4) {
    p = pb_begin();
    pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
    memcpy(p, vs + i, 16);
    p += 4;
    pb_end(p);
  }
  p = pb_begin();
#include "nv2a_ps.inl"
  pb_end(p);

  g_white.pixels = (u32*)MmAllocateContiguousMemoryEx(16, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  for (int i = 0; i < 4; ++i) g_white.pixels[i] = 0xFFFFFFFF;
  g_white.format = nv_format(2, 2);

  g_scale_x = (float)pb_back_buffer_width() / 1280.0f;
  g_scale_y = (float)pb_back_buffer_height() / 720.0f;
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
  u32 hash, draws, prim, vertices;
};
CensusEntry g_census[32];
}  // namespace

void gw_nv2a_draw(u32 opcode, const u32* words, u32 n, const u32* regs, u32 vs_hash) {
  if (!g_ready) return;
  {
    u32 init = opcode == 0x22 ? words[1] : words[0];
    for (CensusEntry& e : g_census) {
      if (!e.hash || e.hash == vs_hash) {
        e.hash = vs_hash;
        ++e.draws;
        e.prim = init & 0x3F;
        e.vertices += init >> 16;
        break;
      }
    }
  }
  const Layout* layout = nullptr;
  for (const Layout& l : kLayouts) {
    if (l.hash == vs_hash) layout = &l;
  }
  if (!layout) return;
  u32 initiator = opcode == 0x22 ? words[1] : words[0];
  u32 prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
  if (source != 2 || count == 0) return;  // auto-indexed only for now
  u32 nv_prim = prim == 1 ? NV097_SET_BEGIN_END_OP_POINTS
              : prim == 4 ? NV097_SET_BEGIN_END_OP_TRIANGLES
              : prim == 13 ? NV097_SET_BEGIN_END_OP_QUADS
              : 0;
  if (!nv_prim) return;
  if (!g_in_frame) begin_frame();
  // pbkit's push buffer only empties at pb_reset (frame start): keep each
  // frame within it. A vertex costs 15 dwords, a draw's state ~30.
  u32 cost = count * 15 * 4 + 160;
  if (g_frame_bytes + cost > kFrameBudget) {
    static u32 reported = 0;
    if (reported++ < 4) {
      gw_log("nv2a: frame %u over budget, skipping %u vertices (shader %08X)\n", g_frames, count, vs_hash);
    }
    return;
  }
  g_frame_bytes += cost;

  // Stream 0 is vertex fetch slot 95.
  u32 vf0 = regs[0x48BE], vf1 = regs[0x48BF];
  u32 vbase = gw_gpu_to_guest(vf0 & ~3u), endian = vf1 & 3;
  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = as_float(regs[0x4000 + i]);
  float vx_scale = as_float(regs[0x210F]), vx_off = as_float(regs[0x2110]);
  float vy_scale = as_float(regs[0x2111]), vy_off = as_float(regs[0x2112]);
  u32 vte = regs[0x2206];

  const Texture* texture = &g_white;
  if (layout->uv_offset >= 0) {
    const Texture* t = texture_from_fetch(&regs[0x4800]);
    if (t) texture = t;
  }
  bind_texture(texture);

  u32 blend = regs[0x2201];
  u32* p = pb_begin();
  p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, blend_factor(blend & 0x1F));
  p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, blend_factor((blend >> 8) & 0x1F));
  if (prim == 1) p = pb_push1(p, NV097_SET_POINT_SIZE, 16);  // 2 px (units of 1/8)
  p = pb_push1(p, NV097_SET_BEGIN_END, nv_prim);
  pb_end(p);

  for (u32 i = 0; i < count; ++i) {
    u32 v = vbase + i * layout->stride;
    auto read = [&](u32 offset) {
      u32 raw;
      memcpy(&raw, gw_host(v + offset), 4);
      return as_float(swap_endian(raw, endian));
    };
    float pos[4] = {read(0), read(4), layout->pos_count > 2 ? read(8) : 0.0f, 1.0f};
    float clip[4];
    for (int r = 0; r < 4; ++r) {
      clip[r] = m[r * 4 + 0] * pos[0] + m[r * 4 + 1] * pos[1] + m[r * 4 + 2] * pos[2] + m[r * 4 + 3] * pos[3];
    }
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
    float sx = x * g_scale_x, sy = y * g_scale_y;
    float w = clip[3];
    float c[4] = {1, 1, 1, 1};
    for (u32 k = 0; k < layout->color_count; ++k) c[k] = read(layout->color_offset + k * 4);
    float u = 0, t = 0;
    if (layout->uv_offset >= 0) {
      u = read(layout->uv_offset);
      t = read(layout->uv_offset + 4);
    }
    static u32 logged = 0;
    if (layout->hash == kVsImage && logged < 8) {
      ++logged;
      gw_log("nv2a: vte %X image v%u pos %d,%d clip %d,%d,%d w*1000=%d uv %d,%d colour*1000 %d,%d,%d\n", vte, i, (int)sx,
             (int)sy, (int)(clip[0] * 1000), (int)(clip[1] * 1000), (int)(clip[2] * 1000), (int)(w * 1000),
             (int)(u * 1000), (int)(t * 1000), (int)(c[0] * 1000), (int)(c[1] * 1000), (int)(c[2] * 1000));
    }
    p = pb_begin();
    p = pb_push4f(p, NV097_SET_VERTEX_DATA4F_M + 3 * 16, c[0], c[1], c[2], c[3]);
    p = pb_push4f(p, NV097_SET_VERTEX_DATA4F_M + 9 * 16, u, t, 0, 1);
    p = pb_push4f(p, NV097_SET_VERTEX_DATA4F_M, sx, sy, 0, 1);
    pb_end(p);
  }
  p = pb_begin();
  p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
  pb_end(p);
}

// Called on each guest swap: shows the frame drawn since the last one.
void gw_nv2a_swap() {
  if (!g_ready) return;
  if (!g_in_frame) begin_frame();
  ++g_frames;
  if (g_frames % 300 == 0) {
    gw_log("nv2a: frame %u vertex shaders:\n", g_frames);
    for (const CensusEntry& e : g_census) {
      if (e.hash) gw_log("  %08X: %u draws, prim %u, %u vertices\n", e.hash, e.draws, e.prim, e.vertices);
    }
  }
  memset(g_census, 0, sizeof(g_census));
  pb_erase_text_screen();
  pb_print("Geometry Wars RE2 - Xbox  frame %u\n", g_frames);
  pb_draw_text_screen();
  // Bounded waits: a stuck NV2A is reported instead of freezing the game.
  gw_nv2a_stage = 3;
  DWORD start = GetTickCount();
  while (pb_busy()) {
    if (GetTickCount() - start > 1000) {
      gw_log("nv2a: GPU still busy after 1 s at frame %u\n", g_frames);
      break;
    }
  }
  gw_nv2a_stage = 4;
  start = GetTickCount();
  while (pb_finished()) {
    if (GetTickCount() - start > 1000) {
      gw_log("nv2a: swap not accepted after 1 s at frame %u\n", g_frames);
      break;
    }
  }
  gw_nv2a_stage = 0;
  g_in_frame = false;
}
