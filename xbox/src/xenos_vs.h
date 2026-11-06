// CPU interpreter for Xenos vertex shaders and a pixel shader analyser
// (xenos_vs.cpp).
#pragma once

#include "runtime.h"

struct XvsProgram;

// Decodes (once per hash) the vertex shader at guest_words.
const XvsProgram* xvs_load(u32 hash, u32 guest_words, u32 dwords);
// Runs it for one vertex: r0.x = vertex index; exports position and
// interpolators 0-7. regs is the GPU register file (constants at 0x4000,
// fetch constants at 0x4800).
bool xvs_run(const XvsProgram* program, const u32* regs, u32 vertex_index, float pos[4], float interp[8][4]);

// The same shader as an NV2A vertex program (xenos_vp.cpp), for a given
// pixel-shader pairing: interpolator uv_i goes to oT0, color_i to oD0.
// divide: perspective divide before the viewport (PA_CL_VTE_CNTL bit 8
// clear). nullptr when the shader can't be translated.
struct XvpProgram {
  u32 key;
  bool ok;
  u32 words[128 * 4];
  u32 instructions;
  u8 const_xenos[60];  // slot 4 + i holds the shader's constant c[const_xenos[i]]
  u32 const_count;
  struct Attribute {
    u32 offset;  // bytes into the vertex
    u8 format;   // Xenos vertex format
    u8 components;
  } attrs[16];
  u32 attr_count;
  u32 stride;  // bytes
  u16 fetch_slot;
};
const XvpProgram* xvp_get(const XvsProgram* program, int uv_i, int color_i, bool divide);

// Records which interpolator feeds the pixel shader's first texture fetch.
void xps_analyse(u32 hash, u32 guest_words, u32 dwords);
bool xps_info(u32 hash, int* uv_interpolator, int* texture_const);

// A pixel shader read as a weighted sum of texture samples: sum over terms
// of (repeat + 1) * texture(fetch constant) * product of the weights, where a
// weight is a pixel shader constant component (index * 4 + component). The
// glow chain's shaders are this shape: 5-tap averages, 15-tap blurs, the sum
// of pyramid levels, the final composite. Offsets of the taps are not kept,
// so a blur becomes one bilinear sample with the blur's total weight.
struct XpsSum {
  static constexpr int kMaxTerms = 8;
  struct Term {
    u8 texture;
    u8 repeat;        // extra identical taps
    u8 weight_count;  // 0..2
    u16 weight[2];
  };
  int count;
  bool log_domain;  // inside a log .. exp pair: constant factors are exponents
  Term terms[kMaxTerms];
};
XpsSum xps_sum_of(const u32* words, u32 dwords);
const XpsSum* xps_sum(u32 hash);
