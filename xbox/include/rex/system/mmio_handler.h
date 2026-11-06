// Xbox stand-in for rex/system/mmio_handler.h: guest GPU register window
// (0x7F000000-0x7FFFFFFF) accesses detected by codegen.
#pragma once

#include <rex/types.h>

namespace rex::runtime {
class MMIOHandler {
 public:
  static MMIOHandler* global_handler();
  void CheckStore(u32 address, u32 value);
  void CheckLoad(u32 address, u32* value);
};
}  // namespace rex::runtime
