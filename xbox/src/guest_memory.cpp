// Guest memory windows on the Xbox. Each window is reserved once at boot so
// nothing else (nxdk's own heap) can land in it; pages are committed as the
// guest allocates. Allocations are simple first-fit ranges with 64 KiB (virtual,
// physical) or 16 byte (system) granularity.
#include "runtime.h"

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

u32 gw_current_caller();  // guest LR of the current thread's last system call
void gw_dump_allocations();

namespace {

struct Range {
  u32 start;
  u32 size;
  u32 caller;  // guest LR of the system call that made it (memory census)
};

constexpr int kMaxRanges = 4096;

struct Window {
  const char* name;
  u32 base;
  u32 size;
  u32 granularity;
  Range used[kMaxRanges];  // sorted by start
  int count;
  CRITICAL_SECTION lock;
};

Window g_virtual{"virtual", kGuestVirtualBase, kGuestVirtualSize, 0x10000};
Window g_physical{"physical", kGuestPhysicalBase, kGuestPhysicalSize, 0x1000};
Window g_stack{"stack", kGuestStackBase, kGuestStackSize, 0x10000};
u32 g_system_next = kGuestSystemBase;
u32 g_system_committed_end = kGuestSystemBase;
CRITICAL_SECTION g_system_lock;
volatile u32 g_committed = 0;
// A census is logged (from the swap, gw_census_due) each time the committed
// total passes another 4 MB above 32 MB.
u32 g_census_at = 32 << 20;
volatile bool g_census_wanted = false;

bool reserve(u32 guest, u32 size) {
  void* want = gw_host(guest);
  void* got = VirtualAlloc(want, size, MEM_RESERVE, PAGE_READWRITE);
  if (got != want) {
    gw_log("reserve %08X+%X at host %p failed (got %p)\n", guest, size, want, got);
    return false;
  }
  return true;
}

// Fresh pages from the kernel are zero-filled. Committing pages that are
// already committed is allowed and keeps their contents.
bool commit(u32 guest, u32 size) {
  u32 end = guest + size;
  u32 newly = 0;
  for (u32 p = guest & ~0xFFFu; p < end;) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(gw_host(p), &info, sizeof(info))) break;
    u32 region_end = gw_guest(info.BaseAddress) + (u32)info.RegionSize;
    if (region_end > end || region_end <= p) region_end = end;
    if (info.State != MEM_COMMIT) newly += region_end - p;
    p = region_end;
  }
  void* want = gw_host(guest);
  void* got = VirtualAlloc(want, size, MEM_COMMIT, PAGE_READWRITE);
  if (got != want) return false;
  g_committed += newly;
  if (g_committed >= g_census_at) {
    g_census_at += 4 << 20;
    g_census_wanted = true;
  }
  return true;
}

void decommit(u32 guest, u32 size) {
  u32 end = guest + size;
  for (u32 p = guest & ~0xFFFu; p < end;) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(gw_host(p), &info, sizeof(info))) break;
    u32 region_end = gw_guest(info.BaseAddress) + (u32)info.RegionSize;
    if (region_end > end || region_end <= p) region_end = end;
    if (info.State == MEM_COMMIT) g_committed -= region_end - p;
    p = region_end;
  }
  VirtualFree(gw_host(guest), size, MEM_DECOMMIT);
}

u32 window_alloc(Window& w, u32 size, u32 align, bool do_commit = true) {
  size = (size + w.granularity - 1) & ~(w.granularity - 1);
  if (align < w.granularity) align = w.granularity;
  EnterCriticalSection(&w.lock);
  u32 candidate = w.base;
  int insert_at = 0;
  for (; insert_at <= w.count; ++insert_at) {
    candidate = (candidate + align - 1) & ~(align - 1);
    u32 limit = insert_at < w.count ? w.used[insert_at].start : w.base + w.size;
    if (candidate + size <= limit && candidate + size > candidate) break;
    if (insert_at < w.count) candidate = w.used[insert_at].start + w.used[insert_at].size;
  }
  u32 result = 0;
  if (insert_at <= w.count && w.count < kMaxRanges && candidate + size <= w.base + w.size) {
    if (!do_commit || commit(candidate, size)) {
      memmove(&w.used[insert_at + 1], &w.used[insert_at], (w.count - insert_at) * sizeof(Range));
      w.used[insert_at] = {candidate, size, gw_current_caller()};
      ++w.count;
      result = candidate;
    }
  }
  LeaveCriticalSection(&w.lock);
  if (!result) {
    MM_STATISTICS stats;
    stats.Length = sizeof(stats);
    MmQueryStatistics(&stats);
    gw_log("%s alloc of %X bytes failed (%u KiB committed by the guest; Xbox: %u of %u pages free, error %u)\n",
           w.name, size, g_committed >> 10, (u32)stats.AvailablePages, (u32)stats.TotalPhysicalPages,
           (u32)GetLastError());
    gw_log_memory("at failure");
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      gw_dump_allocations();
    }
  }
  return result;
}

int window_find(Window& w, u32 guest) {
  for (int i = 0; i < w.count; ++i) {
    if (guest >= w.used[i].start && guest < w.used[i].start + w.used[i].size) return i;
  }
  return -1;
}

void drop_disc_ranges(u32 start, u32 size);

void window_free(Window& w, u32 guest) {
  EnterCriticalSection(&w.lock);
  int i = window_find(w, guest);
  if (i >= 0) {
    drop_disc_ranges(w.used[i].start, w.used[i].size);
    decommit(w.used[i].start, w.used[i].size);
    memmove(&w.used[i], &w.used[i + 1], (w.count - i - 1) * sizeof(Range));
    --w.count;
  }
  LeaveCriticalSection(&w.lock);
}

}  // namespace

bool gw_memory_init() {
  InitializeCriticalSection(&g_virtual.lock);
  InitializeCriticalSection(&g_physical.lock);
  InitializeCriticalSection(&g_stack.lock);
  InitializeCriticalSection(&g_system_lock);
  return reserve(kGuestVirtualBase, kGuestVirtualSize) && reserve(kGuestSystemBase, kGuestSystemSize) &&
         reserve(kGuestStackBase, kGuestStackSize) && reserve(kGuestImageBase, 0x00400000) &&
         reserve(kGuestPhysicalBase, kGuestPhysicalSize) && commit(kGuestImageBase, kGuestImageSize);
}

// Large allocations are committed on first touch (gw_handle_fault): the game
// sizes some pools for the Xbox 360's 512 MiB and uses a fraction of them.
constexpr u32 kLazyThreshold = 0x20000;

u32 gw_virtual_alloc(u32 size, u32 align) { return window_alloc(g_virtual, size, align, size < kLazyThreshold); }
u32 gw_virtual_reserve(u32 size, u32 align) { return window_alloc(g_virtual, size, align, false); }
bool gw_virtual_commit(u32 guest, u32 size) {
  if (size >= kLazyThreshold) return true;
  return gw_commit_range(guest, size);
}
bool gw_commit_range(u32 guest, u32 size) {
  return commit(guest & ~0xFFFu, ((guest & 0xFFF) + size + 0xFFF) & ~0xFFFu);
}
void gw_virtual_decommit(u32 guest, u32 size) { decommit(guest & ~0xFFFu, ((guest & 0xFFF) + size + 0xFFF) & ~0xFFFu); }
u32 gw_allocation_base(u32 guest) {
  Window& w = guest >= kGuestPhysicalBase ? g_physical : g_virtual;
  EnterCriticalSection(&w.lock);
  int i = window_find(w, guest);
  u32 start = i >= 0 ? w.used[i].start : 0;
  LeaveCriticalSection(&w.lock);
  return start;
}
void gw_virtual_free(u32 guest) { window_free(g_virtual, guest); }
u32 gw_physical_alloc(u32 size, u32 align) { return window_alloc(g_physical, size, align, size < kLazyThreshold); }
void gw_physical_free(u32 guest) { window_free(g_physical, guest); }
u32 gw_stack_alloc(u32 size) { return window_alloc(g_stack, size, 0x10000, false); }

u32 gw_virtual_size(u32 guest) {
  Window& w = guest >= kGuestPhysicalBase ? g_physical : g_virtual;
  EnterCriticalSection(&w.lock);
  int i = window_find(w, guest);
  u32 size = i >= 0 ? w.used[i].size - (guest - w.used[i].start) : 0;
  LeaveCriticalSection(&w.lock);
  return size;
}

u32 gw_system_alloc(u32 size, u32 align) {
  EnterCriticalSection(&g_system_lock);
  u32 result = (g_system_next + align - 1) & ~(align - 1);
  u32 end = result + size;
  if (end > kGuestSystemBase + kGuestSystemSize) gw_fatal("system heap exhausted");
  if (end > g_system_committed_end) {
    u32 grow = (end - g_system_committed_end + 0xFFFF) & ~0xFFFFu;
    if (!commit(g_system_committed_end, grow)) gw_fatal("system heap commit failed");
    g_system_committed_end += grow;
  }
  g_system_next = end;
  LeaveCriticalSection(&g_system_lock);
  return result;
}

u32 gw_memory_committed() { return g_committed; }

volatile u32 gw_lazy_faults;
volatile u32 gw_disc_pages;  // pages filled from the disc on touch

// --- disc-backed ranges ----------------------------------------------------------
// A large file read into reserved memory is recorded here instead of being
// performed (the song banks: ~33 MB the game loads whole but only the sound
// hardware reads). A page is read from the disc the first time it is touched.

namespace {

struct DiscRange {
  u32 guest;
  u32 size;
  u64 offset;      // file offset of guest
  HANDLE file;     // our own handle; the game may close its own
  char path[64];
};

constexpr int kMaxDiscRanges = 256;
DiscRange g_disc[kMaxDiscRanges];
CRITICAL_SECTION g_disc_lock;
bool g_disc_lock_ready = false;

// Reads the part of r that falls in [lo, hi) into guest memory.
void fill_range(DiscRange& r, u32 lo, u32 hi) {
  if (lo < r.guest) lo = r.guest;
  if (hi > r.guest + r.size) hi = r.guest + r.size;
  if (lo >= hi) return;
  LARGE_INTEGER pos;
  pos.QuadPart = (LONGLONG)(r.offset + (lo - r.guest));
  SetFilePointerEx(r.file, pos, nullptr, FILE_BEGIN);
  DWORD done = 0;
  ReadFile(r.file, gw_host(lo), hi - lo, &done, nullptr);
  ++gw_disc_pages;
}

void fill_from_disc(u32 page) {
  EnterCriticalSection(&g_disc_lock);
  for (DiscRange& r : g_disc) {
    if (r.file) fill_range(r, page, page + 0x1000);
  }
  LeaveCriticalSection(&g_disc_lock);
}

void drop_disc_ranges(u32 start, u32 size) {
  if (!g_disc_lock_ready) return;
  EnterCriticalSection(&g_disc_lock);
  for (DiscRange& r : g_disc) {
    if (r.file && r.guest >= start && r.guest < start + size) {
      CloseHandle(r.file);
      r = {};
    }
  }
  LeaveCriticalSection(&g_disc_lock);
}

bool page_committed(u32 page) {
  MEMORY_BASIC_INFORMATION info;
  return VirtualQuery(gw_host(page), &info, sizeof(info)) && info.State == MEM_COMMIT;
}

}  // namespace

// Records a read of [offset, offset+size) of host_path into guest memory
// without performing it. Pages that are already committed (an allocation's
// header page, say) are read now; the rest wait for their first touch.
bool gw_map_from_disc(u32 guest, u32 size, const char* host_path, u64 offset) {
  if (!g_disc_lock_ready) {
    InitializeCriticalSection(&g_disc_lock);
    g_disc_lock_ready = true;
  }
  if (!gw_allocation_base(guest) || strlen(host_path) >= sizeof(DiscRange::path)) return false;
  HANDLE file = CreateFileA(host_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  EnterCriticalSection(&g_disc_lock);
  for (DiscRange& r : g_disc) {
    if (r.file) continue;
    r = {guest, size, offset, file, {}};
    strcpy(r.path, host_path);
    for (u32 p = guest & ~0xFFFu; p < guest + size; p += 0x1000) {
      if (page_committed(p)) fill_range(r, p, p + 0x1000);
    }
    LeaveCriticalSection(&g_disc_lock);
    gw_log("disc-backed: %s +%X, %u KiB at %08X\n", host_path, (u32)offset, size >> 10, guest);
    return true;
  }
  LeaveCriticalSection(&g_disc_lock);
  CloseHandle(file);
  return false;
}

// A guest memcpy whose source lies inside one disc-backed range becomes a new
// disc-backed range at the destination, so a whole song bank moves from the
// loader's buffer to its final one without being read.
bool gw_disc_copy(u32 dst, u32 src, u32 n) {
  if (!g_disc_lock_ready) return false;
  char path[64];
  u64 offset = 0;
  bool found = false;
  EnterCriticalSection(&g_disc_lock);
  for (DiscRange& r : g_disc) {
    if (r.file && src >= r.guest && src + n <= r.guest + r.size) {
      strcpy(path, r.path);
      offset = r.offset + (src - r.guest);
      found = true;
      break;
    }
  }
  LeaveCriticalSection(&g_disc_lock);
  return found && gw_map_from_disc(dst, n, path, offset);
}

// Access violation in a reserved guest allocation: commit the 16 KiB around
// it (clamped to the allocation), fill newly committed pages that belong to a
// disc-backed range, and let the access retry.
bool gw_handle_fault(u32 host_address) {
  u32 guest = host_address - (u32)GW_MEMBASE;
  Window* w = nullptr;
  if (guest >= kGuestVirtualBase && guest < kGuestVirtualBase + kGuestVirtualSize) w = &g_virtual;
  if (guest >= kGuestPhysicalBase && guest < kGuestPhysicalBase + kGuestPhysicalSize) w = &g_physical;
  if (guest >= kGuestStackBase && guest < kGuestStackBase + kGuestStackSize) w = &g_stack;
  if (!w) return false;
  EnterCriticalSection(&w->lock);
  int i = window_find(*w, guest);
  bool ok = false;
  if (i >= 0) {
    u32 start = guest & ~0x3FFFu, end = start + 0x4000;
    u32 lo = w->used[i].start, hi = w->used[i].start + w->used[i].size;
    if (start < lo) start = lo & ~0xFFFu;
    if (end > hi) end = (hi + 0xFFF) & ~0xFFFu;
    ok = true;
    for (u32 page = start; page < end && ok; page += 0x1000) {
      if (page_committed(page)) continue;
      ok = commit(page, 0x1000);
      if (ok && g_disc_lock_ready) fill_from_disc(page);
    }
    ++gw_lazy_faults;
  }
  LeaveCriticalSection(&w->lock);
  if (i >= 0 && !ok) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      gw_log("commit-on-touch at %08X failed: out of memory\n", guest);
      gw_log_memory("at failure");
      gw_dump_allocations();
    }
  }
  return ok;
}

void gw_log_memory(const char* when);
void gw_dump_allocations();
u32 gw_current_caller();

// --- dispatch -----------------------------------------------------------------

GwDispatchEntry gw_dispatch_table[1u << kGwDispatchBits];

PPCFunc* gw_lookup(u32 guest_address) { return gw_dispatch_find(guest_address); }

bool gw_census_due() {
  if (!g_census_wanted) return false;
  g_census_wanted = false;
  return true;
}

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  gw_fatal("Call to unregistered guest function %08X", guest_address);
}
}  // namespace rex::runtime

// --- libm gaps -------------------------------------------------------------------
// SIMDe's vector round-to-nearest-even falls back to roundevenf, which nxdk's
// libm does not provide.
extern "C" float roundevenf(float x) {
  float f = __builtin_floorf(x);
  float d = x - f;
  if (d > 0.5f || (d == 0.5f && __builtin_fmodf(f, 2.0f) != 0.0f)) f += 1.0f;
  return f;
}

void gw_log_memory(const char* when) {
  MM_STATISTICS stats;
  stats.Length = sizeof(stats);
  MmQueryStatistics(&stats);
  gw_log("memory %s: free %u KiB of %u KiB; committed virtual %u KiB, image %u KiB, pool %u KiB, "
         "stacks %u KiB, cache %u KiB; guest %u KiB\n",
         when, (u32)stats.AvailablePages * 4, (u32)stats.TotalPhysicalPages * 4,
         (u32)stats.VirtualMemoryBytesCommitted >> 10, (u32)stats.ImagePagesCommitted * 4,
         (u32)stats.PoolPagesCommitted * 4, (u32)stats.StackPagesCommitted * 4,
         (u32)stats.CachePagesCommitted * 4, g_committed >> 10);
}

namespace {
u32 committed_in(u32 start, u32 size) {
  u32 total = 0, end = start + size;
  for (u32 p = start; p < end;) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(gw_host(p), &info, sizeof(info))) break;
    u32 region_end = gw_guest(info.BaseAddress) + (u32)info.RegionSize;
    if (region_end > end || region_end <= p) region_end = end;
    if (info.State == MEM_COMMIT) total += region_end - p;
    p = region_end;
  }
  return total;
}
}  // namespace

// Largest committed guest allocations, with the code that requested them.
void gw_dump_allocations() {
  Window* windows[3] = {&g_virtual, &g_physical, &g_stack};
  for (Window* w : windows) {
    EnterCriticalSection(&w->lock);
    u32 total = 0;
    for (int i = 0; i < w->count; ++i) {
      u32 c = committed_in(w->used[i].start, w->used[i].size);
      total += c;
      if (c >= 0x10000) {
        gw_log("  %s %08X size %X committed %u KiB, requested from %08X\n", w->name, w->used[i].start,
               w->used[i].size, c >> 10, w->used[i].caller);
      }
    }
    gw_log("  %s total: %u allocations, %u KiB committed\n", w->name, w->count, total >> 10);
    LeaveCriticalSection(&w->lock);
  }
}
