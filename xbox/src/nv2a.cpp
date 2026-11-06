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

#ifndef MASK
#define MASK(mask, value) (((value) << (__builtin_ffs(mask) - 1)) & (mask))
#endif

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
constexpr u32 kArenaBytes = 768 * 1024;
u8* g_arena;
u32 g_arena_used;  // bytes
// Draw with translated NV2A vertex programs (else the CPU interpreter).
bool g_use_gpu_vertex = true;
float g_scale_x, g_scale_y;
// Our own glow over each frame (on unless make GLOW=n; see apply_glow).
#ifdef GW_GLOW
constexpr bool kGlow = true;
#else
constexpr bool kGlow = false;
#endif
constexpr float kGlowStrength = 0.7f;
constexpr u32 kGlowProgramSlot = 128;  // nv2a_glow_vs.vs.cg (after the translated programs)
u32 g_frames;

struct Texture {
  u32 key;  // gpu address ^ size ^ format
  u32* pixels;
  u32 format;
  u32 filter;  // NV097_SET_TEXTURE_FILTER
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

u32 nv_format(u32 w, u32 h, u32 colour = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, u32 levels = 1) {
  return 1 | (2 << 4) | NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE | (colour << 8) | (levels << 16) |
         (log2_ceil(w) << 20) | (log2_ceil(h) << 24);
}

// Converts a k_8_8_8_8 texture to an NV2A texture, cached by address: sides
// rounded to powers of two (down where up would grow them by over half), at
// most 1024. Up to 256x256 as swizzled A8R8G8B8; larger ones (fonts, title
// art) as DXT5, which keeps their full resolution, and so the text sharp, in
// a quarter of the memory.
Texture* texture_from_fetch(const u32* f) {
  u32 format = f[1] & 0x3F;
  if (format != 6) {
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
  // Round a side down instead when rounding up would grow it by over half
  // (300 -> 256, not 512): converted textures are a large share of memory.
  if (tw > width + width / 2) tw >>= 1;
  if (th > height + height / 2) th >>= 1;
  if (tw > 1024) tw = 1024;
  if (th > 1024) th = 1024;
  const bool compressed = (tw > 256 || th > 256) && tw >= 4 && th >= 4;
  // Mipmaps, each level a 2x2 average of the one above: the game draws its
  // text and art smaller than the textures (1280-wide art on a 640-wide
  // screen), and without them the minified glyphs alias. Down to 4 texels a
  // side for DXT5 (one block), 1 otherwise.
  const u32 smallest = compressed ? 4 : 1;
  u32 levels = 1, bytes = 0;
  for (u32 w = tw, h = th;; w >>= 1, h >>= 1) {
    bytes += compressed ? w * h : w * h * 4;
    if (w / 2 < smallest || h / 2 < smallest || levels == 10) break;
    ++levels;
  }
  u32* pixels = (u32*)MmAllocateContiguousMemoryEx(bytes, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (!pixels) return nullptr;
  bool tiled = f[0] >> 31;
  u32 pitch = ((f[0] >> 22) & 0x1FF) * 32;
  u32 endian = (f[1] >> 6) & 3;
  const u8* src = gw_host(gw_gpu_to_guest(base));
  // The source pixel for (x, y) of the converted texture (nearest).
  auto texel = [&](u32 x, u32 y) {
    const u32 sx = x * width / tw, sy = y * height / th;
    const u32 offset = tiled ? (tiled_column(sx, sy, 2, tiled_row(sy, pitch, 2)) >> 2) * 4 : (sy * pitch + sx) * 4;
    u32 raw;
    memcpy(&raw, src + offset, 4);
    return swap_endian(raw, endian);
  };
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
  slot->format = compressed ? nv_format(tw, th, NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8, levels)
                            : nv_format(tw, th, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, levels);
  slot->filter = levels > 1 ? 0x02062000 : 0x02022000;  // trilinear with mipmaps, else bilinear
  gw_log("nv2a: texture %08X %ux%u%s -> %ux%u%s, %u levels\n", base, width, height, tiled ? " tiled" : "", tw, th,
         compressed ? " DXT5" : "", levels);
  // Large images (the title screens) are only read by the GPU: once
  // converted, the 360-format original gives its memory back. A later touch
  // recommits it as zeros.
  u32 guest = gw_gpu_to_guest(base);
  if (gw_allocation_base(guest) == guest && gw_virtual_size(guest) >= 0x40000) {
    gw_virtual_decommit(guest, gw_virtual_size(guest));
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
  gw_nv2a_stage = 1;
  const u64 start = gw_wait_begin();
  pb_reset();  // waits for the GPU to finish the previous frame
  gw_wait_end(start, 0, "renderer: GPU finishing the previous frame");
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
  g_arena_used = 0;
  g_in_frame = true;
}

void bind_texture(const Texture* t) {
  u32* p = pb_begin();
  p = pb_push2(p, NV097_SET_TEXTURE_OFFSET, (u32)t->pixels & 0x03FFFFFF, t->format);
  p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, 0x00030303);  // clamp
  p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0x4003FFC0);
  p = pb_push1(p, NV097_SET_TEXTURE_FILTER, t->filter);
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
  if (kGlow) pb_extra_buffers(2);  // the glow's work buffers
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
  if (kGlow) {
    u32 glow_vs[] = {
#include "nv2a_glow_vs.inl"
    };
    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, kGlowProgramSlot);
    pb_end(p);
    for (u32 i = 0; i < sizeof(glow_vs) / 4; i += 4) {
      p = pb_begin();
      pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
      memcpy(p, glow_vs + i, 16);
      p += 4;
      pb_end(p);
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
  u32 hash, draws, prim, vertices, gpu, cpu;  // gpu/cpu: vertices drawn by each path
  u32 skip;  // why the last skipped draw was skipped (SkipReason), with its detail
  u32 skip_detail;
};
CensusEntry g_census[32];
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
    u32* p = pb_begin();
    for (u32 a = attr; a < attr + 4; ++a) {
      u32 format = formats[a] ? formats[a] : NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
      p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + a * 4, format);
      if (formats[a]) p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + a * 4, offsets[a]);
    }
    pb_end(p);
  }
}

void draw_arrays(u32 nv_prim, u32 count) {
  // DRAW_ARRAYS takes at most 256 vertices; 252 keeps quads and triangles
  // whole.
  for (u32 start = 0; start < count;) {
    u32 n = count - start > 252 ? 252 : count - start;
    u32* p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, nv_prim);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS, ((n - 1) << 24) | start);
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
    start += n;
  }
}

u32 g_program_start = 0xFFFFFFFF;  // transform program start slot in use
u32 g_loaded_key = 0;              // translated program loaded at slot 8

void set_program_start(u32 start) {
  if (g_program_start == start) return;
  u32* p = pb_begin();
  p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, start);
  pb_end(p);
  g_program_start = start;
}

// Uploads constants (4 floats each) from slot first.
void upload_constants(u32 first, const u32* values, u32 count) {
  for (u32 done = 0; done < count;) {
    u32 n = count - done > 8 ? 8 : count - done;
    u32* p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 + first + done);
    pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, n * 4);
    memcpy(p, values + done * 4, n * 16);
    p += n * 4;
    pb_end(p);
    done += n;
  }
}

u32 float_bits(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  return u;
}

// Room for 'bytes' more in the vertex arena. When a busy frame fills it, the
// GPU finishes what it has been given (pb_reset waits for that; render state
// carries over) and the arena starts again from the beginning: one stall
// instead of dropping draws or falling back to the CPU interpreter.
bool arena_room(u32 bytes) {
  if (g_arena_used + bytes <= kArenaBytes) return true;
  if (bytes > kArenaBytes) return false;
  static u32 reported = 0;
  if (reported++ < 4) gw_log("nv2a: frame %u filled the vertex arena, waiting for the GPU\n", g_frames);
  const u64 start = gw_wait_begin();
  pb_reset();
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
// 'rects': a rectangle list, drawn as quads. tex_w, tex_h scale the texture
// coordinates (a linear texture is addressed in texels).
bool draw_on_gpu(const XvpProgram* gp, const u32* regs, u32 nv_prim, u32 count, u32 source, u32 index_base,
                 u32 index_endian, bool index32, u32 vte, bool rects = false, float tex_w = 1, float tex_h = 1) {
  if (rects && (count % 3 || !gp->attr_count || gp->stride > 64)) return false;
  const u32 out_count = rects ? count / 3 * 4 : count;
  u32 bytes = out_count * gp->stride;
  if (!arena_room(bytes)) return false;
  if (g_loaded_key != gp->key) {
    u32* p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 8);
    pb_end(p);
    for (u32 i = 0; i < gp->instructions; i += 16) {
      u32 n = gp->instructions - i > 16 ? 16 : gp->instructions - i;
      p = pb_begin();
      for (u32 k = 0; k < n; ++k) {
        pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
        memcpy(p, &gp->words[(i + k) * 4], 16);
        p += 4;
      }
      pb_end(p);
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
  float vx_scale = as_float(regs[0x210F]), vx_off = as_float(regs[0x2110]);
  float vy_scale = as_float(regs[0x2111]), vy_off = as_float(regs[0x2112]);
  c[12] = float_bits((vte & 1 ? vx_scale : 1.0f) * g_scale_x);
  c[13] = float_bits((vte & 4 ? vy_scale : 1.0f) * g_scale_y);
  c[14] = float_bits((vte & 2 ? vx_off : 0.0f) * g_scale_x);
  c[15] = float_bits((vte & 8 ? vy_off : 0.0f) * g_scale_y);
  for (u32 i = 0; i < gp->const_count; ++i) memcpy(&c[(4 + i) * 4], &regs[0x4000 + gp->const_xenos[i] * 4], 16);
  upload_constants(0, c, 4 + gp->const_count);

  // Vertices: the shader's stream, in draw order, words swapped to host order.
  u32 f0 = regs[0x4800 + gp->fetch_slot * 2], f1 = regs[0x4800 + gp->fetch_slot * 2 + 1];
  u32 stream = gw_gpu_to_guest(f0 & ~3u), endian = f1 & 3;
  u32 words = gp->stride / 4;
  u32* out = (u32*)(g_arena + g_arena_used);
  u32 corners[3][16];  // a rectangle's three corners, host order
  for (u32 i = 0; i < count; ++i) {
    u32 index = i;
    if (source == 0) {
      if (index32) {
        u32 raw;
        memcpy(&raw, gw_host(index_base + i * 4), 4);
        index = swap_endian(raw, index_endian);
      } else {
        u16 h;
        memcpy(&h, gw_host(index_base + i * 2), 2);
        index = index_endian ? (u16)((h << 8) | (h >> 8)) : h;
      }
    }
    const u32* in = (const u32*)gw_host(stream + index * gp->stride);
    u32* dst = rects ? corners[i % 3] : out;
    for (u32 k = 0; k < words; ++k) dst[k] = swap_endian(in[k], endian);
    if (!rects) {
      out += words;
    } else if (i % 3 == 2) {
      const u32* v[3] = {corners[0], corners[1], corners[2]};
      rect_to_quad(v, gp, out);
      out += 4 * words;
    }
  }

  u32 formats[16] = {}, offsets[16] = {};
  u32 base = ((u32)g_arena & 0x03FFFFFF) + g_arena_used;
  for (u32 a = 0; a < gp->attr_count; ++a) {
    const XvpProgram::Attribute& at = gp->attrs[a];
    formats[a] = at.format == 6
                     ? NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL | (4 << 4) | (gp->stride << 8)
                     : NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F | ((u32)at.components << 4) | (gp->stride << 8);
    offsets[a] = base + at.offset;
  }
  set_arrays(formats, offsets);
  g_arena_used += (bytes + 15) & ~15u;
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
  u32* p = pb_begin();
  p = pb_push2(p, NV097_SET_TEXTURE_OFFSET + o, (u32)pixels & 0x03FFFFFF,
               1 | (2 << 4) | NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE |
                   (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5 << 8) | (1 << 16));
  p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS + o, 0x00030303);  // clamp
  p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + o, 0x4003FFC0);
  p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1 + o, pitch << 16);
  p = pb_push1(p, NV097_SET_TEXTURE_FILTER + o, 0x02022000);  // bilinear
  p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT + o, (width << 16) | height);
  pb_end(p);
}

// Combiner constants for the four-texture sum: stage 0's factors weigh units
// 0 and 1, stage 1's and stage 2's second factors units 2 and 3 (where
// fp20compiler put them; each stage keeps its own factors).
void set_sum_weights(const float w[4]) {
  auto colour = [](float v) {
    const u32 b = v <= 0 ? 0 : v >= 1 ? 255 : (u32)(v * 255.0f + 0.5f);
    return b << 24 | b << 16 | b << 8 | b;
  };
  u32* p = pb_begin();
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR0 + 0 * 4, colour(w[0]));
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + 0 * 4, colour(w[1]));
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + 1 * 4, colour(w[2]));
  p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + 2 * 4, colour(w[3]));
  pb_end(p);
}

void use_sum_combiner() {
  u32* p = pb_begin();
#include "nv2a_sum_ps.inl"
  p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
               MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_EACH_STAGE) |
                   MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_EACH_STAGE) |
                   MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 3));
  pb_end(p);
}

void use_default_combiner() {
  u32* p = pb_begin();
#include "nv2a_ps.inl"
  for (u32 unit = 1; unit < 4; ++unit) p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + unit * 64, 0);
  pb_end(p);
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
               const float offsets[4][2], const float weights[4]) {
  for (u32 unit = 0; unit < 4; ++unit) bind_linear(unit, from, from_w, from_h, pitch);
  set_sum_weights(weights);
  float uv[4][4];
  for (int k = 0; k < 4; ++k) {
    uv[k][0] = offsets[k][0];
    uv[k][1] = offsets[k][1];
    uv[k][2] = offsets[k][0] + scale * w;
    uv[k][3] = offsets[k][1] + scale * h;
  }
  glow_quad(0, 0, w, h, uv);
}

void apply_glow() {
  const u32 width = pb_back_buffer_width(), height = pb_back_buffer_height(), pitch = pb_back_buffer_pitch();
  const float w = (float)(width / 4), h = (float)(height / 4);
  const void* picture = pb_back_buffer();
  const void* small = pb_extra_buffer(0);
  const void* blurred = pb_extra_buffer(1);
  set_program_start(kGlowProgramSlot);
  use_sum_combiner();
  u32* p = pb_begin();
  p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
  pb_end(p);

  // 1. A quarter of the size: four bilinear taps, each the average of 2x2,
  // cover the 4x4 block (output pixel x reads around source 4x + 1.5).
  pb_target_extra_buffer(0);
  const float quarter[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  const float even[4] = {0.25f, 0.25f, 0.25f, 0.25f};
  glow_pass(picture, width, height, pitch, w, h, 4, quarter, even);
  // 2, 3. Blur across, then down: four bilinear taps, eight texels.
  pb_target_extra_buffer(1);
  const float across[4][2] = {{-2.5f, 0}, {-0.5f, 0}, {1.5f, 0}, {3.5f, 0}};
  glow_pass(small, width, height, pitch, w, h, 1, across, even);
  pb_target_extra_buffer(0);
  const float down[4][2] = {{0, -2.5f}, {0, -0.5f}, {0, 1.5f}, {0, 3.5f}};
  glow_pass(blurred, width, height, pitch, w, h, 1, down, even);

  // 4. Added back over the picture, enlarged (bilinear).
  pb_target_back_buffer();
  p = pb_begin();
  p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
  p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, 0x0001);  // one
  p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, 0x0001);  // one
  p = pb_push1(p, NV097_SET_BLEND_EQUATION, 0x8006);      // add
  pb_end(p);
  const float none[4][2] = {};
  const float strength[4] = {kGlowStrength, 0, 0, 0};
  glow_pass(small, width, height, pitch, (float)width, (float)height, 0.25f, none, strength);

  use_default_combiner();
}

}  // namespace

void gw_nv2a_draw(u32 opcode, const u32* words, u32 n, const u32* regs, u32 vs_hash, const XvsProgram* vs,
                  u32 ps_hash) {
  if (!g_ready) return;
  u32 initiator = opcode == 0x22 ? words[1] : words[0];
  u32 prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
  g_census_entry = nullptr;
  for (CensusEntry& e : g_census) {
    if (!e.hash || e.hash == vs_hash) {
      e.hash = vs_hash;
      ++e.draws;
      e.prim = prim;
      e.vertices += count;
      g_census_entry = &e;
      break;
    }
  }
  if (!vs || count == 0) return;
  // Colour draws into the main 1280-wide scene only: the glow chain, the
  // grid's own target, resolves (mode 6) and clears (mode 5) wait.
  if ((regs[0x2208] & 7) != 4 || (regs[0x2000] & 0x3FFF) != 1280) {
    // detail: mode << 16 | surface pitch
    return note_skip(kNotMainTarget, (regs[0x2208] & 7) << 16 | (regs[0x2000] & 0x3FFF));
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

  float vx_scale = as_float(regs[0x210F]), vx_off = as_float(regs[0x2110]);
  float vy_scale = as_float(regs[0x2111]), vy_off = as_float(regs[0x2112]);
  u32 vte = regs[0x2206];

  // Pixel shader: which interpolator is the texture coordinate.
  int uv_i = -1, tex_const = 0;
  xps_info(ps_hash, &uv_i, &tex_const);
  const Texture* texture = &g_white;
  if (prim != 1) {
    // A draw whose texture can't be reproduced yet (render targets, float
    // formats, computed coordinates) is skipped: many use replace blending
    // and would paint flat shapes over the scene.
    if (uv_i == -2) return note_skip(kComputedUv, 0);
    if (uv_i >= 0) {
      const Texture* t = texture_from_fetch(&regs[0x4800 + tex_const * 6]);
      // The in-level grid's lines (vertex shader 8E1A294A) are never drawn;
      // the likely reason (to be confirmed by the census's skip reasons) is
      // that they sample one of the game's float render targets (formats 26,
      // 54), which are not reproduced yet. With a white texel instead they
      // keep their own colours: the grid without its glow.
      const u32 texture_format = regs[0x4800 + tex_const * 6 + 1] & 0x3F;
      if (!t && vs_hash == 0x8E1A294A && (texture_format == 26 || texture_format == 54)) t = &g_white;
      if (!t) return note_skip(kTextureFormat, texture_format);
      texture = t;
    }
  }
  if (uv_i < 0) uv_i = -1;
  int color_i = uv_i == 0 ? 1 : 0;
  bind_texture(texture);

  u32 blend = regs[0x2201];
  u32* p = pb_begin();
  p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, blend_factor(blend & 0x1F));
  p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, blend_factor((blend >> 8) & 0x1F));
  // Xenos BlendOp (add, subtract, min, max, reverse subtract) -> GL equation.
  static const u32 kEquations[5] = {0x8006, 0x800A, 0x8007, 0x8008, 0x800B};
  u32 op = (blend >> 5) & 7;
  p = pb_push1(p, NV097_SET_BLEND_EQUATION, op < 5 ? kEquations[op] : 0x8006);
  if (prim == 1) p = pb_push1(p, NV097_SET_POINT_SIZE, 16);  // 2 px (units of 1/8)
  pb_end(p);

  // Preferred: the shader as an NV2A vertex program.
  if (g_use_gpu_vertex) {
    const XvpProgram* gp = xvp_get(vs, uv_i, color_i, !(vte & 0x100));
    if (gp && draw_on_gpu(gp, regs, nv_prim, count, source, index_base, index_endian, index32, vte)) {
      if (g_census_entry) g_census_entry->gpu += count;
      return;
    }
  }

  // Fallback: interpret the shader on the CPU, draw pre-transformed vertices.
  if (!arena_room(count * 20)) {
    static u32 reported = 0;
    if (reported++ < 4) {
      gw_log("nv2a: frame %u draw of %u vertices is larger than the vertex arena (shader %08X)\n", g_frames, count,
             vs_hash);
    }
    return;
  }
  if (g_census_entry) g_census_entry->cpu += count;
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
    xvs_run(vs, regs, index, clip, interp);
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
    const float* c = interp[color_i];
    float u = uv_i >= 0 ? interp[uv_i][0] : 0, t = uv_i >= 0 ? interp[uv_i][1] : 0;
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
  draw_arrays(nv_prim, count);
}

// Called on each guest swap: shows the frame drawn since the last one.
void gw_nv2a_swap() {
  if (!g_ready) return;
  if (!g_in_frame) begin_frame();
  ++g_frames;
  if (g_frames % 300 == 0) {
    gw_log("nv2a: frame %u vertex shaders:\n", g_frames);
    for (const CensusEntry& e : g_census) {
      if (e.hash) {
        gw_log("  %08X: %u draws, prim %u, %u vertices (%u on the GPU, %u on the CPU)%s%s %X\n", e.hash, e.draws,
               e.prim, e.vertices, e.gpu, e.cpu, e.skip ? "; skipped: " : "", kSkipNames[e.skip],
               e.skip_detail);
      }
    }
  }
  memset(g_census, 0, sizeof(g_census));
  if (gw_census_due()) {
    gw_log_memory("census");
    gw_dump_allocations();
  }
  if (kGlow) apply_glow();
  pb_erase_text_screen();
  pb_print("Geometry Wars RE2 - Xbox  frame %u\n", g_frames);
  pb_draw_text_screen();
  // The GPU draws this frame while the game builds the next one: the next
  // frame's pb_reset (begin_frame) is what waits for it, so the vertex arena
  // is only reused once the GPU is done with it. Here the flip is queued,
  // which only waits while all three buffers are still on their way to the
  // screen. Bounded: a stuck NV2A is reported instead of freezing the game.
  gw_nv2a_stage = 4;
  const u64 queued = gw_wait_begin();
  DWORD start = GetTickCount();
  while (pb_finished()) {
    if (GetTickCount() - start > 1000) {
      gw_log("nv2a: swap not accepted after 1 s at frame %u\n", g_frames);
      break;
    }
  }
  gw_wait_end(queued, 0, "renderer: flip queue full");
  gw_nv2a_stage = 0;
  g_in_frame = false;
}
