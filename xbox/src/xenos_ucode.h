// Decoded Xenos vertex shader instructions, shared by the CPU interpreter
// (xenos_vs.cpp) and the NV2A vertex program translator (xenos_vp.cpp).
#pragma once

#include "runtime.h"

namespace xenos {

constexpr int kMaxInstructions = 64;

struct Source {
  bool temp;     // temporary register, else constant
  bool absolute;
  bool negate;
  u8 index;
  u8 swizzle[4];  // component read for each result component
};

struct Instruction {
  bool fetch;
  // vertex fetch
  bool mini, is_signed, normalized;
  u8 dst, src, src_component, format;
  u16 slot;
  u32 stride, offset;
  u8 dst_select[4];  // 0-3 component, 4 zero, 5 one, 7 keep
  // ALU
  bool predicated, pred_condition, export_data, vclamp, sclamp;
  u8 vop, sop, vdst, sdst, vmask, smask;
  Source s[3];
};

struct Program {
  u32 hash;
  int count;
  Instruction ins[kMaxInstructions];
};

}  // namespace xenos
