// Host mirrors of guest vertex buffers that native code fills.
//
// The native particle and starfield code (src/particle_native.cpp,
// src/stars_native.cpp) writes its vertices in the NV2A's order (host-order
// floats) into a contiguous host buffer standing for the start of a guest
// vertex buffer, instead of big-endian into the guest buffer. A draw whose
// vertices all lie in such a mirror is drawn from it in place
// (src/nv2a.cpp): no byte-swapping copy into the vertex arena, which was
// most of the command processor's time. Only the first bytes of each guest
// buffer are mirrored (the guest buffers are sized for the worst case); the
// writers fall back to the guest buffer past the mirror, and a draw reaching
// past it is copied as before, its mirrored part from the mirror.
#include "runtime.h"

#include <xboxkrnl/xboxkrnl.h>

namespace {
struct Mirror {
  u32 guest, size;
  u8* host;
};
constexpr int kMaxMirrors = 32;
Mirror g_mirrors[kMaxMirrors];
volatile long g_count = 0;
}  // namespace

u8* gw_vertex_mirror_add(u32 guest, u32 size) {
  for (int i = 0; i < g_count; ++i) {
    if (g_mirrors[i].guest == guest) return g_mirrors[i].host;
  }
  if (g_count >= kMaxMirrors) return nullptr;
  u8* host = (u8*)MmAllocateContiguousMemoryEx(size, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (!host) {
    gw_log("vertex mirror: no contiguous memory for %u bytes\n", size);
    return nullptr;
  }
  const int i = (int)g_count;
  g_mirrors[i] = {guest, size, host};
  InterlockedIncrement(&g_count);  // published after the entry is filled
  return host;
}

u8* gw_vertex_mirror(u32 guest, u32 bytes) {
  const int n = (int)g_count;
  for (int i = 0; i < n; ++i) {
    const Mirror& m = g_mirrors[i];
    if (guest - m.guest < m.size && bytes <= m.size - (guest - m.guest)) return m.host + (guest - m.guest);
  }
  return nullptr;
}

