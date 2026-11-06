// Memory exports: virtual and physical allocation, queries, XamAlloc.
#include "runtime.h"

namespace {
constexpr u32 kMemCommit = 0x1000;
constexpr u32 kMemReserve = 0x2000;
constexpr u32 kMemDecommit = 0x4000;
constexpr u32 kMemRelease = 0x8000;
constexpr u32 kMemLargePages = 0x20000000;  // 64 KiB pages
constexpr u32 kPageReadWrite = 0x04;
}  // namespace

// NtAllocateVirtualMemory(base_ptr, size_ptr, type, protect, debug)
GW_EXPORT(NtAllocateVirtualMemory) {
  u32 base_ptr = GW_ARG(3), size_ptr = GW_ARG(4), type = GW_ARG(5);
  u32 want = gw_load32(base_ptr);
  u32 size = gw_load32(size_ptr);
  if (!size) GW_RETURN(X_STATUS_INVALID_PARAMETER);
  size = (size + 0xFFF) & ~0xFFFu;
  if (want) {
    // Commit inside an existing reservation.
    if (!gw_allocation_base(want)) {
      gw_log("NtAllocateVirtualMemory at unreserved %08X+%X\n", want, size);
      GW_RETURN(X_STATUS_NO_MEMORY);
    }
    if (type & kMemLargePages) {
      // 64 KiB pages: the commit covers whole pages, and the game's heap
      // relies on the rounded-up tail being usable.
      u32 end = (want + size + 0xFFFF) & ~0xFFFFu;
      want &= ~0xFFFFu;
      size = end - want;
    }
    if ((type & kMemCommit) && !gw_virtual_commit(want, size)) GW_RETURN(X_STATUS_NO_MEMORY);
    gw_log("NtAllocateVirtualMemory commit %08X+%X type %X\n", want, size, type);
    gw_store32(base_ptr, want & ~0xFFFu);
    gw_store32(size_ptr, size);
    GW_RETURN(X_STATUS_SUCCESS);
  }
  u32 result = (type & kMemCommit) ? gw_virtual_alloc(size, 0x10000) : gw_virtual_reserve(size, 0x10000);
  gw_log("NtAllocateVirtualMemory %X type %X protect %X -> %08X\n", size, type, GW_ARG(6), result);
  if (!result) GW_RETURN(X_STATUS_NO_MEMORY);
  gw_store32(base_ptr, result);
  gw_store32(size_ptr, (size + 0xFFFF) & ~0xFFFFu);
  GW_RETURN(X_STATUS_SUCCESS);
}

// NtFreeVirtualMemory(base_ptr, size_ptr, type, debug)
GW_EXPORT(NtFreeVirtualMemory) {
  u32 base_ptr = GW_ARG(3), size_ptr = GW_ARG(4), type = GW_ARG(5);
  u32 address = gw_load32(base_ptr);
  u32 size = size_ptr ? gw_load32(size_ptr) : 0;
  if (!gw_allocation_base(address)) GW_RETURN(X_STATUS_INVALID_PARAMETER);
  if (type & kMemRelease) {
    gw_virtual_free(address);
  } else if (type & kMemDecommit) {
    gw_virtual_decommit(address, size ? size : gw_virtual_size(address));
  }
  GW_RETURN(X_STATUS_SUCCESS);
}

// NtQueryVirtualMemory(address, info_ptr, region_type)
GW_EXPORT(NtQueryVirtualMemory) {
  u32 address = GW_ARG(3), info = GW_ARG(4);
  u32 alloc_base = gw_allocation_base(address);
  u32 page = address & ~0xFFFu;
  gw_store32(info + 0x00, page);
  gw_store32(info + 0x04, alloc_base);
  gw_store32(info + 0x08, alloc_base ? kPageReadWrite : 0);
  gw_store32(info + 0x0C, alloc_base ? gw_virtual_size(page) : 0x1000);
  gw_store32(info + 0x10, alloc_base ? kMemCommit : 0x10000 /* MEM_FREE */);
  gw_store32(info + 0x14, alloc_base ? kPageReadWrite : 0);
  gw_store32(info + 0x18, alloc_base ? 0x20000 /* MEM_PRIVATE */ : 0);
  GW_RETURN(X_STATUS_SUCCESS);
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min, max, alignment)
// The in-level grid's line vertex buffers (92048 vertices of 28 bytes,
// written by the grid job in src/grid_native.cpp, drawn by vertex shader
// 8E1A294A into the glow layer) are triple-buffered and rewritten in full
// every frame: 7.5 MB that does not fit in 64 MB with the rest of a level.
// src/nv2a.cpp does not draw the glow layer yet, so all three share one
// buffer. Give them their own memory again once the grid is drawn.
namespace {
constexpr u32 kGridLineBufferSize = 0x2753C0;
struct SharedBuffer {
  u32 address, users;
};
SharedBuffer g_grid_lines;

u32 alloc_grid_line_buffer(u32 alignment) {
  if (!g_grid_lines.address) g_grid_lines.address = gw_physical_alloc(kGridLineBufferSize, alignment);
  if (g_grid_lines.address) ++g_grid_lines.users;
  return g_grid_lines.address;
}

// True when the free was of the shared grid line buffer (released on its last user).
bool free_grid_line_buffer(u32 address) {
  if (!g_grid_lines.address || address != g_grid_lines.address) return false;
  if (--g_grid_lines.users == 0) {
    gw_physical_free(address);
    g_grid_lines.address = 0;
  }
  return true;
}
}  // namespace

GW_EXPORT(MmAllocatePhysicalMemoryEx) {
  u32 size = GW_ARG(4), alignment = GW_ARG(8);
  alignment = alignment > 0x1000 ? alignment : 0x1000;
  u32 result = size == kGridLineBufferSize ? alloc_grid_line_buffer(alignment) : gw_physical_alloc(size, alignment);
  gw_log("MmAllocatePhysicalMemoryEx(%X, align %X) -> %08X lr %08X\n", size, alignment, result, (u32)ctx.lr);
  static int traced_128k = 0;
  if (size >= 0x100000 || (size == 0x20000 && traced_128k++ % 50 == 0)) gw_log_backtrace(ctx);
  GW_RETURN(result);
}

GW_EXPORT(MmFreePhysicalMemory) {
  if (!free_grid_line_buffer(GW_ARG(4))) gw_physical_free(GW_ARG(4));
}

GW_EXPORT(MmGetPhysicalAddress) {
  u32 address = GW_ARG(3);
  GW_RETURN(address >= kGuestPhysicalBase ? gw_guest_to_gpu(address) : address);
}

GW_EXPORT(MmQueryAddressProtect) { GW_RETURN(gw_allocation_base(GW_ARG(3)) ? kPageReadWrite : 0); }

// XamAlloc(flags, size, out_ptr)
GW_EXPORT(XamAlloc) {
  u32 size = GW_ARG(4);
  u32 result = gw_virtual_alloc(size ? size : 1, 0x10000);
  gw_log("XamAlloc(%X, %X) -> %08X\n", GW_ARG(3), size, result);
  gw_store32(GW_ARG(5), result);
  GW_RETURN(result ? X_ERROR_SUCCESS : 14 /* ERROR_OUTOFMEMORY */);
}

GW_EXPORT(XamFree) {
  gw_virtual_free(GW_ARG(3));
  GW_RETURN(X_ERROR_SUCCESS);
}

GW_EXPORT(RtlFillMemoryUlong) {
  u32 dest = GW_ARG(3), length = GW_ARG(4), pattern = GW_ARG(5);
  // The pattern is stored as the guest would store it (big-endian word).
  for (u32 i = 0; i + 4 <= length; i += 4) gw_store32(dest + i, pattern);
}

GW_EXPORT(RtlCompareMemoryUlong) {
  u32 source = GW_ARG(3), length = GW_ARG(4), pattern = GW_ARG(5);
  u32 matched = 0;
  while (matched + 4 <= length && gw_load32(source + matched) == pattern) matched += 4;
  GW_RETURN(matched);
}
