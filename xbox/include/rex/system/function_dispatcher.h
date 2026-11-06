// Xbox stand-in for rex/system/function_dispatcher.h.
#pragma once

#include <rex/types.h>
#include <rex/ppc/func.h>

namespace rex::runtime {
class IModuleRegistrar {
 public:
  virtual void SetFunction(u32 guest_address, PPCFunc* host) = 0;
};
}  // namespace rex::runtime
