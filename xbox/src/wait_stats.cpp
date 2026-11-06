// Where the time goes when nothing runs: every guest wait (events,
// semaphores, sleeps) and the renderer's own waits are timed and summed by
// thread and call site; every 10 seconds the biggest are logged, e.g.
//   waits: thread 4 at 82095A10 KeWaitForSingleObject: 598 waits, 6123 ms (10.2 ms each)
// The cycle counter is read with rdtsc (733 MHz on the Xbox).
#include "runtime.h"

#include <windows.h>

namespace {

struct Entry {
  u32 caller;
  u32 thread;
  const char* what;
  u32 count;
  u64 cycles;
};

constexpr int kEntries = 128;
Entry g_entries[kEntries];
CRITICAL_SECTION g_lock;
volatile LONG g_ready;
u64 g_window_start;

inline u64 cycles_now() { return __builtin_ia32_rdtsc(); }

void ensure_lock() {
  if (InterlockedCompareExchange(&g_ready, 1, 0) == 0) {
    InitializeCriticalSection(&g_lock);
    g_window_start = cycles_now();
    g_ready = 2;
  }
  while (g_ready != 2) SwitchToThread();
}

}  // namespace

u64 gw_wait_begin() { return cycles_now(); }

void gw_wait_end(u64 start, u32 caller, const char* what) {
  const u64 spent = cycles_now() - start;
  const u32 thread = GetCurrentThreadId();
  ensure_lock();
  EnterCriticalSection(&g_lock);
  Entry* slot = nullptr;
  for (Entry& e : g_entries) {
    if (e.count && e.caller == caller && e.thread == thread && e.what == what) {
      slot = &e;
      break;
    }
    if (!e.count && !slot) slot = &e;
  }
  if (slot) {
    if (!slot->count) *slot = {caller, thread, what, 0, 0};
    ++slot->count;
    slot->cycles += spent;
  }
  LeaveCriticalSection(&g_lock);
}

// Logs and resets the totals once 10 seconds have passed (called from the
// vblank thread).
void gw_wait_report() {
  if (g_ready != 2) return;
  constexpr u64 kHz = 733333333ull;
  const u64 now = cycles_now();
  if (now - g_window_start < 10 * kHz) return;
  EnterCriticalSection(&g_lock);
  const u64 window = now - g_window_start;
  g_window_start = now;
  Entry top[12] = {};
  for (const Entry& e : g_entries) {
    if (!e.count) continue;
    for (int i = 0; i < 12; ++i) {
      if (e.cycles > top[i].cycles) {
        for (int k = 11; k > i; --k) top[k] = top[k - 1];
        top[i] = e;
        break;
      }
    }
  }
  memset(g_entries, 0, sizeof(g_entries));
  LeaveCriticalSection(&g_lock);
  gw_log("waits over %u ms (thread ids are the host's):\n", (u32)(window * 1000 / kHz));
  for (const Entry& e : top) {
    if (!e.count) break;
    const u32 ms = (u32)(e.cycles * 1000 / kHz);
    gw_log("  thread %u at %08X %s: %u waits, %u ms (%u us each)\n", e.thread, e.caller, e.what, e.count, ms,
           (u32)(e.cycles * 1000000 / kHz / e.count));
  }
  gw_fault_report();
}
