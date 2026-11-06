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

// Records which interpolator feeds the pixel shader's first texture fetch.
void xps_analyse(u32 hash, u32 guest_words, u32 dwords);
bool xps_info(u32 hash, int* uv_interpolator, int* texture_const);
