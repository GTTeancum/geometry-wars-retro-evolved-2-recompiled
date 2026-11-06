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

namespace {

constexpr int kMaxInstructions = 64;

struct Instruction {
  bool fetch;
  u32 w[3];
};

struct Program {
  u32 hash;
  int count;
  Instruction ins[kMaxInstructions];
};

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

inline float swz(const float* v, u32 sw, int i) { return v[(bits(sw, i * 2, 2) + i) & 3]; }

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
    if (p.count < kMaxInstructions) p.ins[p.count++] = {fetch, {w[0], w[1], w[2]}};
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
  float r[64][4] = {};
  r[0][0] = (float)vertex_index;
  float prev_scalar = 0;
  bool predicate = false;
  u32 last_fetch_address = 0, last_fetch_stride = 0, last_fetch_endian = 0;
  pos[0] = pos[1] = pos[2] = 0;
  pos[3] = 1;
  for (int i = 0; i < 8; ++i) interp[i][0] = interp[i][1] = interp[i][2] = 0, interp[i][3] = 1;

  for (int n = 0; n < p.count; ++n) {
    const u32* w = p.ins[n].w;
    if (p.ins[n].fetch) {
      if (bits(w[0], 0, 5) != 0) continue;  // texture fetch in a vertex shader
      u32 dst = bits(w[0], 12, 6), src = bits(w[0], 5, 6);
      bool mini = bits(w[1], 30, 1);
      u32 format = bits(w[1], 16, 6);
      bool is_signed = bits(w[1], 13, 1), normalized = !bits(w[1], 12, 1);
      u32 offset = bits(w[2], 8, 23) * 4;
      if (!mini) {
        u32 slot = bits(w[0], 20, 5) * 3 + bits(w[0], 25, 2);
        u32 f0 = regs[0x4800 + slot * 2], f1 = regs[0x4800 + slot * 2 + 1];
        last_fetch_address = gw_gpu_to_guest(f0 & ~3u);
        last_fetch_endian = f1 & 3;
        last_fetch_stride = bits(w[2], 0, 8) * 4;
        u32 index = (u32)r[src][bits(w[0], 30, 2)];
        last_fetch_address += index * last_fetch_stride;
      }
      float v[4];
      fetch_element(last_fetch_address + offset, last_fetch_endian, format, is_signed, normalized, v);
      u32 dst_swz = bits(w[1], 0, 12);
      for (int c = 0; c < 4; ++c) {
        u32 s = bits(dst_swz, c * 3, 3);
        if (s == 7) continue;  // keep
        r[dst][c] = s < 4 ? v[s] : s == 4 ? 0.0f : 1.0f;
      }
      continue;
    }

    // ALU
    u32 d0 = w[0], d1 = w[1], d2 = w[2];
    bool predicated = bits(d1, 27, 1);
    if (predicated && predicate != (bool)bits(d1, 28, 1)) continue;
    u32 vdst = bits(d0, 0, 6), sdst = bits(d0, 8, 6);
    bool export_data = bits(d0, 15, 1);
    u32 vmask = bits(d0, 16, 4), smask = bits(d0, 20, 4);
    bool vclamp = bits(d0, 24, 1), sclamp = bits(d0, 25, 1);
    u32 sop = bits(d0, 26, 6), vop = bits(d2, 24, 5);
    bool abs_const = bits(d0, 7, 1);
    u32 swz_bits[3] = {bits(d1, 16, 8), bits(d1, 8, 8), bits(d1, 0, 8)};
    bool neg[3] = {(bool)bits(d1, 26, 1), (bool)bits(d1, 25, 1), (bool)bits(d1, 24, 1)};
    u32 reg[3] = {bits(d2, 16, 8), bits(d2, 8, 8), bits(d2, 0, 8)};
    bool sel[3] = {(bool)bits(d2, 31, 1), (bool)bits(d2, 30, 1), (bool)bits(d2, 29, 1)};
    float src[3][4];
    for (int s = 0; s < 3; ++s) {
      float raw[4];
      bool absolute;
      if (sel[s]) {
        memcpy(raw, r[reg[s] & 0x3F], 16);
        absolute = reg[s] & 0x80;
      } else {
        for (int c = 0; c < 4; ++c) raw[c] = as_float(regs[0x4000 + reg[s] * 4 + c]);
        absolute = abs_const;
      }
      for (int c = 0; c < 4; ++c) {
        float v = swz(raw, swz_bits[s], c);
        if (absolute) v = fabsf(v);
        if (neg[s]) v = -v;
        src[s][c] = v;
      }
    }
    // Vector op
    float vr[4] = {0, 0, 0, 0};
    const float *a = src[0], *b = src[1], *c3 = src[2];
    switch (vop) {
      case 0: for (int c = 0; c < 4; ++c) vr[c] = a[c] + b[c]; break;
      case 1: for (int c = 0; c < 4; ++c) vr[c] = a[c] * b[c]; break;
      case 2: for (int c = 0; c < 4; ++c) vr[c] = a[c] >= b[c] ? a[c] : b[c]; break;
      case 3: for (int c = 0; c < 4; ++c) vr[c] = a[c] < b[c] ? a[c] : b[c]; break;
      case 4: for (int c = 0; c < 4; ++c) vr[c] = a[c] == b[c] ? 1.0f : 0.0f; break;
      case 5: for (int c = 0; c < 4; ++c) vr[c] = a[c] > b[c] ? 1.0f : 0.0f; break;
      case 6: for (int c = 0; c < 4; ++c) vr[c] = a[c] >= b[c] ? 1.0f : 0.0f; break;
      case 7: for (int c = 0; c < 4; ++c) vr[c] = a[c] != b[c] ? 1.0f : 0.0f; break;
      case 8: for (int c = 0; c < 4; ++c) vr[c] = a[c] - floorf(a[c]); break;
      case 9: for (int c = 0; c < 4; ++c) vr[c] = truncf(a[c]); break;
      case 10: for (int c = 0; c < 4; ++c) vr[c] = floorf(a[c]); break;
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
      case 20: case 21: case 22: case 23: break;  // setp_*_push: predicates unused here
      default: break;
    }
    if (vclamp) for (int c = 0; c < 4; ++c) vr[c] = vr[c] < 0 ? 0 : vr[c] > 1 ? 1 : vr[c];

    // Scalar op: one operand is src3's swizzled .w (component 3); two
    // operands are .w and .x.
    float sa = src[2][3], sb = src[2][0];
    float sr = prev_scalar;
    bool scalar_write = smask != 0;
    switch (sop) {
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
      case 11: sr = sa - floorf(sa); break;
      case 12: sr = truncf(sa); break;
      case 13: sr = floorf(sa); break;
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
      case 50: sr = prev_scalar; break;  // retain_prev
      default: break;
    }
    if (sclamp) sr = sr < 0 ? 0 : sr > 1 ? 1 : sr;
    prev_scalar = sr;

    auto write = [&](u32 dst, u32 mask, const float* value) {
      float* out;
      if (export_data) {
        if (dst == 62) out = pos;
        else if (dst < 8) out = interp[dst];
        else return;
      } else {
        out = r[dst];
      }
      for (int c = 0; c < 4; ++c) {
        if (mask & (1u << c)) out[c] = value[c];
      }
    };
    if (vmask) write(vdst, vmask, vr);
    if (scalar_write) {
      float s4[4] = {sr, sr, sr, sr};
      write(sdst, smask, s4);
    }
  }
  return true;
}
