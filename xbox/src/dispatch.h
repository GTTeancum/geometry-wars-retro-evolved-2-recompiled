// Guest function address -> recompiled host function, for indirect calls.
// An open-addressed hash table (128 KB for the game's ~6000 functions) in
// place of one entry per guest instruction slot (1.4 MB). Filled at boot from
// PPCFuncMappings (src/main.cpp); shared by the generated code (gen/pch.h)
// and the runtime.
#pragma once

#include <rex/ppc/func.h>

struct GwDispatchEntry {
  uint32_t guest;
  PPCFunc* host;
};

constexpr uint32_t kGwDispatchBits = 14;
constexpr uint32_t kGwDispatchMask = (1u << kGwDispatchBits) - 1;
extern GwDispatchEntry gw_dispatch_table[1u << kGwDispatchBits];

inline uint32_t gw_dispatch_hash(uint32_t guest) { return ((guest >> 2) * 2654435761u) >> (32 - kGwDispatchBits); }

inline PPCFunc* gw_dispatch_find(uint32_t guest) {
  for (uint32_t i = gw_dispatch_hash(guest);; i = (i + 1) & kGwDispatchMask) {
    const GwDispatchEntry& e = gw_dispatch_table[i];
    if (e.guest == guest) return e.host;
    if (!e.guest) return nullptr;
  }
}

inline void gw_dispatch_add(uint32_t guest, PPCFunc* host) {
  uint32_t i = gw_dispatch_hash(guest);
  while (gw_dispatch_table[i].guest && gw_dispatch_table[i].guest != guest) i = (i + 1) & kGwDispatchMask;
  gw_dispatch_table[i] = {guest, host};
}
