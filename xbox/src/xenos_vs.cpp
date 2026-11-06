// CPU interpreter for Xenos vertex shaders, so the NV2A renderer can draw
// any of the game's vertex shaders (the title screen warps its geometry with
// sin/cos in the shader). Decoding follows xbox/tools/xenos_disasm.py, which
// follows ReXGlue's ucode.h. Programs are decoded once, when the game loads
// them, and cached by microcode hash.
//
// Pixel shaders are only analysed: which interpolator feeds the texture
// fetch (its texture coordinates) and which texture fetch constant it uses.
#include "xenos_vs.h"

#include <math.h>

#include "xenos_ucode.h"

namespace {

// Instructions are decoded once at load (xenos_ucode.h); the per-vertex
// loop only does maths.
using xenos::Instruction;
using xenos::kMaxInstructions;
using xenos::Program;
using xenos::Source;

constexpr int kMaxPrograms = 48;
Program g_programs[kMaxPrograms];
int g_program_count;

struct PixelInfo {
  u32 hash;
  int uv_interpolator;  // -1: no texture fetch
  int texture_const;
};
PixelInfo g_pixel[48];
int g_pixel_count;

u32 bits(u32 v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

// Walks the control flow and collects the exec'd instructions in order.
template <typename F>
void walk(const u32* words, u32 dwords, F&& visit) {
  // Control flow occupies the dwords before the first exec'd instruction.
  u32 first_exec = 0xFFFF;
  for (u32 pair = 0; pair * 3 + 2 < dwords && pair * 3 < first_exec * 3; ++pair) {
    u32 a = words[pair * 3], b = words[pair * 3 + 1], c = words[pair * 3 + 2];
    u32 cf[2][2] = {{a, b & 0xFFFF}, {(b >> 16) | (c << 16), c >> 16}};
    for (auto& d : cf) {
      u32 op = bits(d[1], 12, 4);
      bool exec = op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 13 || op == 14;
      if (!exec) continue;
      u32 addr = bits(d[0], 0, 12), n = bits(d[0], 12, 3), seq = bits(d[0], 16, 12);
      if (addr < first_exec) first_exec = addr;
      for (u32 k = 0; k < n; ++k) {
        u32 base = (addr + k) * 3;
        if (base + 3 > dwords) return;
        visit(bits(seq, k * 2, 1) != 0, words + base);
      }
      if (op == 2 || op == 4 || op == 6 || op == 14) return;  // *_end
    }
  }
}

float as_float(u32 v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}

u32 swap_endian(u32 v, u32 endian) {
  switch (endian & 3) {
    case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}

// Reads one vertex element; returns the component count.
int fetch_element(u32 address, u32 endian, u32 format, bool is_signed, bool normalized, float out[4]) {
  auto word = [&](u32 offset) {
    u32 raw;
    memcpy(&raw, gw_host(address + offset), 4);
    return swap_endian(raw, endian);
  };
  out[0] = out[1] = out[2] = 0;
  out[3] = 1;
  switch (format) {
    case 36: out[0] = as_float(word(0)); return 1;
    case 37: for (int i = 0; i < 2; ++i) out[i] = as_float(word(i * 4)); return 2;
    case 57: for (int i = 0; i < 3; ++i) out[i] = as_float(word(i * 4)); return 3;
    case 38: for (int i = 0; i < 4; ++i) out[i] = as_float(word(i * 4)); return 4;
    case 6: {  // 8_8_8_8: x in the low byte after the swap
      u32 v = word(0);
      for (int i = 0; i < 4; ++i) {
        u32 b = (v >> (i * 8)) & 0xFF;
        float f = is_signed ? (float)(int8_t)b : (float)b;
        out[i] = normalized ? f / (is_signed ? 127.0f : 255.0f) : f;
      }
      return 4;
    }
    case 25: case 26: {  // 16_16, 16_16_16_16
      int n = format == 25 ? 2 : 4;
      for (int i = 0; i < n; ++i) {
        u32 v = word((i / 2) * 4);
        u32 h = (i & 1) ? (v >> 16) : (v & 0xFFFF);
        float f = is_signed ? (float)(int16_t)h : (float)h;
        out[i] = normalized ? f / (is_signed ? 32767.0f : 65535.0f) : f;
      }
      return n;
    }
    default: return 0;
  }
}

// floorf without the library call (|v| < 2^31 here).
inline float fast_floor(float v) {
  float f = (float)(int)v;
  return f > v ? f - 1.0f : f;
}

inline float swz(const float* v, u32 sw, int i) { return v[(bits(sw, i * 2, 2) + i) & 3]; }

void decode(Instruction& in, bool fetch, const u32* w) {
  in = {};
  in.fetch = fetch;
  if (fetch) {
    if (bits(w[0], 0, 5) != 0) {  // texture fetch: unused in vertex shaders
      in.format = 0xFF;
      return;
    }
    in.dst = (u8)bits(w[0], 12, 6);
    in.src = (u8)bits(w[0], 5, 6);
    in.src_component = (u8)bits(w[0], 30, 2);
    in.mini = bits(w[1], 30, 1);
    in.format = (u8)bits(w[1], 16, 6);
    in.is_signed = bits(w[1], 13, 1);
    in.normalized = !bits(w[1], 12, 1);
    in.offset = bits(w[2], 8, 23) * 4;
    in.stride = bits(w[2], 0, 8) * 4;
    in.slot = (u16)(bits(w[0], 20, 5) * 3 + bits(w[0], 25, 2));
    for (int c = 0; c < 4; ++c) in.dst_select[c] = (u8)bits(w[1], c * 3, 3);
    return;
  }
  u32 d0 = w[0], d1 = w[1], d2 = w[2];
  in.predicated = bits(d1, 27, 1);
  in.pred_condition = bits(d1, 28, 1);
  in.vdst = (u8)bits(d0, 0, 6);
  in.sdst = (u8)bits(d0, 8, 6);
  in.export_data = bits(d0, 15, 1);
  in.vmask = (u8)bits(d0, 16, 4);
  in.smask = (u8)bits(d0, 20, 4);
  in.vclamp = bits(d0, 24, 1);
  in.sclamp = bits(d0, 25, 1);
  in.sop = (u8)bits(d0, 26, 6);
  in.vop = (u8)bits(d2, 24, 5);
  bool abs_const = bits(d0, 7, 1);
  const u32 swz[3] = {bits(d1, 16, 8), bits(d1, 8, 8), bits(d1, 0, 8)};
  const bool neg[3] = {(bool)bits(d1, 26, 1), (bool)bits(d1, 25, 1), (bool)bits(d1, 24, 1)};
  const u32 reg[3] = {bits(d2, 16, 8), bits(d2, 8, 8), bits(d2, 0, 8)};
  const bool sel[3] = {(bool)bits(d2, 31, 1), (bool)bits(d2, 30, 1), (bool)bits(d2, 29, 1)};
  for (int s = 0; s < 3; ++s) {
    Source& src = in.s[s];
    src.temp = sel[s];
    src.index = (u8)(sel[s] ? (reg[s] & 0x3F) : reg[s]);
    src.absolute = sel[s] ? (reg[s] & 0x80) != 0 : abs_const;
    src.negate = neg[s];
    for (int c = 0; c < 4; ++c) src.swizzle[c] = (u8)((bits(swz[s], c * 2, 2) + c) & 3);
  }
}

}  // namespace

const XvsProgram* xvs_load(u32 hash, u32 guest_words, u32 dwords) {
  for (int i = 0; i < g_program_count; ++i) {
    if (g_programs[i].hash == hash) return (const XvsProgram*)&g_programs[i];
  }
  if (g_program_count >= kMaxPrograms || dwords > 1024) return nullptr;
  static u32 words[1024];
  for (u32 i = 0; i < dwords; ++i) words[i] = gw_load32(guest_words + i * 4);
  Program& p = g_programs[g_program_count];
  p.hash = hash;
  p.count = 0;
  walk(words, dwords, [&](bool fetch, const u32* w) {
    if (p.count < kMaxInstructions) decode(p.ins[p.count++], fetch, w);
  });
  ++g_program_count;
  return (const XvsProgram*)&p;
}

void xps_analyse(u32 hash, u32 guest_words, u32 dwords) {
  for (int i = 0; i < g_pixel_count; ++i) {
    if (g_pixel[i].hash == hash) return;
  }
  if (g_pixel_count >= 48 || dwords > 1024) return;
  static u32 words[1024];
  for (u32 i = 0; i < dwords; ++i) words[i] = gw_load32(guest_words + i * 4);
  PixelInfo info{hash, -1, 0};
  walk(words, dwords, [&](bool fetch, const u32* w) {
    if (fetch && bits(w[0], 0, 5) == 1 && info.uv_interpolator < 0) {
      // Texture fetch from an input register: interpolator n arrives in rn.
      // Only an input register counts; a computed coordinate (a high temp)
      // is treated as untextured for now.
      int reg = (int)bits(w[0], 5, 6);
      info.uv_interpolator = reg < 8 ? reg : -2;
      info.texture_const = (int)bits(w[0], 20, 5);
    }
  });
  g_pixel[g_pixel_count++] = info;
}

bool xps_info(u32 hash, int* uv_interpolator, int* texture_const) {
  for (int i = 0; i < g_pixel_count; ++i) {
    if (g_pixel[i].hash == hash) {
      *uv_interpolator = g_pixel[i].uv_interpolator;
      *texture_const = g_pixel[i].texture_const;
      return true;
    }
  }
  return false;
}

bool xvs_run(const XvsProgram* program, const u32* regs, u32 vertex_index, float pos[4], float interp[8][4]) {
  const Program& p = *(const Program*)program;
  typedef float __attribute__((__may_alias__)) alias_float;
  const alias_float* consts = (const alias_float*)&regs[0x4000];
  // Shaders write a temporary before reading it; only r0 (the vertex index)
  // needs a value.
  float r[64][4];
  r[0][0] = (float)vertex_index;
  r[0][1] = r[0][2] = r[0][3] = 0;
  float prev_scalar = 0;
  bool predicate = false;
  u32 fetch_address = 0, fetch_endian = 0;
  pos[0] = pos[1] = pos[2] = 0;
  pos[3] = 1;
  for (int i = 0; i < 8; ++i) interp[i][0] = interp[i][1] = interp[i][2] = 0, interp[i][3] = 1;

  for (int n = 0; n < p.count; ++n) {
    const Instruction& in = p.ins[n];
    if (in.fetch) {
      if (in.format == 0xFF) continue;
      if (!in.mini) {
        u32 f0 = regs[0x4800 + in.slot * 2], f1 = regs[0x4800 + in.slot * 2 + 1];
        fetch_endian = f1 & 3;
        u32 index = (u32)r[in.src][in.src_component];
        fetch_address = gw_gpu_to_guest(f0 & ~3u) + index * in.stride;
      }
      float v[4];
      fetch_element(fetch_address + in.offset, fetch_endian, in.format, in.is_signed, in.normalized, v);
      for (int c = 0; c < 4; ++c) {
        u8 s = in.dst_select[c];
        if (s == 7) continue;  // keep
        r[in.dst][c] = s < 4 ? v[s] : s == 4 ? 0.0f : 1.0f;
      }
      continue;
    }

    if (in.predicated && predicate != in.pred_condition) continue;
    float src[3][4];
    for (int s = 0; s < 3; ++s) {
      const Source& so = in.s[s];
      const float* raw = so.temp ? r[so.index] : (const float*)&consts[so.index * 4];
      for (int c = 0; c < 4; ++c) {
        float v = raw[so.swizzle[c]];
        if (so.absolute) v = fabsf(v);
        src[s][c] = so.negate ? -v : v;
      }
    }
    // Vector op
    float vr[4] = {0, 0, 0, 0};
    const float *a = src[0], *b = src[1], *c3 = src[2];
    switch (in.vop) {
      case 0: for (int c = 0; c < 4; ++c) vr[c] = a[c] + b[c]; break;
      case 1: for (int c = 0; c < 4; ++c) vr[c] = a[c] * b[c]; break;
      case 2: for (int c = 0; c < 4; ++c) vr[c] = a[c] >= b[c] ? a[c] : b[c]; break;
      case 3: for (int c = 0; c < 4; ++c) vr[c] = a[c] < b[c] ? a[c] : b[c]; break;
      case 4: for (int c = 0; c < 4; ++c) vr[c] = a[c] == b[c] ? 1.0f : 0.0f; break;
      case 5: for (int c = 0; c < 4; ++c) vr[c] = a[c] > b[c] ? 1.0f : 0.0f; break;
      case 6: for (int c = 0; c < 4; ++c) vr[c] = a[c] >= b[c] ? 1.0f : 0.0f; break;
      case 7: for (int c = 0; c < 4; ++c) vr[c] = a[c] != b[c] ? 1.0f : 0.0f; break;
      case 8: for (int c = 0; c < 4; ++c) vr[c] = a[c] - fast_floor(a[c]); break;
      case 9: for (int c = 0; c < 4; ++c) vr[c] = truncf(a[c]); break;
      case 10: for (int c = 0; c < 4; ++c) vr[c] = fast_floor(a[c]); break;
      case 11: for (int c = 0; c < 4; ++c) vr[c] = a[c] * b[c] + c3[c]; break;
      case 12: for (int c = 0; c < 4; ++c) vr[c] = a[c] == 0.0f ? b[c] : c3[c]; break;
      case 13: for (int c = 0; c < 4; ++c) vr[c] = a[c] >= 0.0f ? b[c] : c3[c]; break;
      case 14: for (int c = 0; c < 4; ++c) vr[c] = a[c] > 0.0f ? b[c] : c3[c]; break;
      case 15: vr[0] = vr[1] = vr[2] = vr[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
      case 16: vr[0] = vr[1] = vr[2] = vr[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
      case 17: vr[0] = vr[1] = vr[2] = vr[3] = a[0] * b[0] + a[1] * b[1] + c3[0]; break;
      case 19: {
        float m = a[0];
        for (int c = 1; c < 4; ++c) m = a[c] > m ? a[c] : m;
        vr[0] = vr[1] = vr[2] = vr[3] = m;
        break;
      }
      default: break;  // setp_*_push and kill ops: unused by the vertex shaders
    }
    if (in.vclamp) for (int c = 0; c < 4; ++c) vr[c] = vr[c] < 0 ? 0 : vr[c] > 1 ? 1 : vr[c];

    // Scalar op: one operand is src3's swizzled .w; two operands are .w, .x.
    float sa = src[2][3], sb = src[2][0];
    float sr = prev_scalar;
    switch (in.sop) {
      case 0: sr = sa + sb; break;
      case 1: sr = sa + prev_scalar; break;
      case 2: sr = sa * sb; break;
      case 3: sr = sa * prev_scalar; break;
      case 5: sr = sa >= sb ? sa : sb; break;
      case 6: sr = sa < sb ? sa : sb; break;
      case 7: sr = sa == 0.0f ? 1.0f : 0.0f; break;
      case 8: sr = sa > 0.0f ? 1.0f : 0.0f; break;
      case 9: sr = sa >= 0.0f ? 1.0f : 0.0f; break;
      case 10: sr = sa != 0.0f ? 1.0f : 0.0f; break;
      case 11: sr = sa - fast_floor(sa); break;
      case 12: sr = truncf(sa); break;
      case 13: sr = fast_floor(sa); break;
      case 14: sr = exp2f(sa); break;
      case 15: case 16: sr = log2f(sa); break;
      case 17: case 18: case 19: sr = 1.0f / sa; break;
      case 20: case 21: case 22: sr = 1.0f / sqrtf(sa); break;
      case 25: sr = sa - sb; break;
      case 26: sr = sa - prev_scalar; break;
      case 27: predicate = sa == 0.0f; sr = predicate ? 0.0f : 1.0f; break;
      case 28: predicate = sa != 0.0f; sr = predicate ? 0.0f : 1.0f; break;
      case 29: predicate = sa > 0.0f; sr = predicate ? 0.0f : 1.0f; break;
      case 30: predicate = sa >= 0.0f; sr = predicate ? 0.0f : 1.0f; break;
      case 40: sr = sqrtf(sa); break;
      case 48: sr = sinf(sa); break;
      case 49: sr = cosf(sa); break;
      default: break;  // retain_prev and the rest keep the previous result
    }
    if (in.sclamp) sr = sr < 0 ? 0 : sr > 1 ? 1 : sr;
    prev_scalar = sr;

    auto target = [&](u32 dst) -> float* {
      if (!in.export_data) return r[dst];
      if (dst == 62) return pos;
      return dst < 8 ? interp[dst] : nullptr;
    };
    if (in.vmask) {
      if (float* out = target(in.vdst)) {
        for (int c = 0; c < 4; ++c) {
          if (in.vmask & (1u << c)) out[c] = vr[c];
        }
      }
    }
    if (in.smask) {
      if (float* out = target(in.sdst)) {
        for (int c = 0; c < 4; ++c) {
          if (in.smask & (1u << c)) out[c] = sr;
        }
      }
    }
  }
  return true;
}
