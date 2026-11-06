// Xbox stand-in for rex/image_info.h.
#pragma once

#include <rex/types.h>
#include <rex/ppc/func.h>

namespace rex {

struct PPCCodegenFlags {
  bool skip_lr;
  bool ctr_as_local;
  bool xer_as_local;
  bool reserved_as_local;
  bool skip_msr;
  bool cr_as_local;
  bool non_argument_as_local;
  bool non_volatile_as_local;
};

struct PPCImageInfo {
  u64 code_base;
  u64 code_size;
  u64 image_base;
  u64 image_size;
  PPCFuncMapping* func_mappings;
  u32 rexcrt_heap;
  PPCCodegenFlags codegen_flags;
};

}  // namespace rex
