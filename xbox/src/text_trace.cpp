// Test only (make TEXT_TRACE=y): which game code draws text with the vector
// font, and what: each text routine's callers (return address), the string
// (or format) they pass and how often, logged every 5 s. Used to find the
// screens' drawing functions (the initials entry draws over the game-over
// screen with the game's own text routines).
#ifdef GW_TEXT_TRACE
#include "runtime.h"

#include <windows.h>

namespace {

struct Site {
  u32 routine, lr, calls, r7;
  float f[4];
  char text[28];
};
Site g_sites[96];
CRITICAL_SECTION g_lock;
bool g_ready;
DWORD g_last;

void note(u32 routine, PPCContext& ctx) {
  if (!g_ready) {
    InitializeCriticalSection(&g_lock);
    g_ready = true;
    g_last = GetTickCount();
  }
  EnterCriticalSection(&g_lock);
  const u32 lr = (u32)ctx.lr;
  Site* free_site = nullptr;
  bool found = false;
  for (Site& s : g_sites) {
    if (s.calls && s.routine == routine && s.lr == lr) {
      ++s.calls;
      found = true;
      break;
    }
    if (!s.calls && !free_site) free_site = &s;
  }
  if (!found && free_site) {
    free_site->routine = routine;
    free_site->lr = lr;
    free_site->calls = 1;
    free_site->r7 = ctx.r7.u32;
    free_site->f[0] = (float)ctx.f1.f64;
    free_site->f[1] = (float)ctx.f2.f64;
    free_site->f[2] = (float)ctx.f3.f64;
    free_site->f[3] = (float)ctx.f4.f64;
    const u32 text = ctx.r8.u32;
    u32 n = 0;
    if (text >= 0x82000000u && text < 0x82220000u || text >= 0x60000000u && text < 0x72000000u) {
      for (; n < sizeof(free_site->text) - 1; ++n) {
        const u8 c = gw_load8(text + n);
        if (!c) break;
        free_site->text[n] = c >= 32 && c < 127 ? (char)c : '.';
      }
    }
    free_site->text[n] = 0;
  }
  if (GetTickCount() - g_last > 5000) {
    g_last = GetTickCount();
    gw_log("text trace at frame %u:\n", gw_stats.swaps);
    for (Site& s : g_sites) {
      if (!s.calls) continue;
      gw_log("  %08X from %08X: %u calls, \"%s\" (x %d y %d size %d/1000 %d/1000, colours %08X)\n", s.routine, s.lr,
             s.calls, s.text, (int)s.f[0], (int)s.f[1], (int)(s.f[2] * 1000), (int)(s.f[3] * 1000), s.r7);
      s.calls = 0;
    }
  }
  LeaveCriticalSection(&g_lock);
}

}  // namespace

#define TRACE(addr)                                                    \
  extern "C" void __imp__sub_##addr(PPCContext& ctx, uint8_t* base);   \
  extern "C" void sub_##addr(PPCContext& ctx, uint8_t* base) {         \
    note(0x##addr, ctx);                                               \
    __imp__sub_##addr(ctx, base);                                      \
  }

TRACE(82076130)
// (82076200 is hooked by src/live_scores.cpp: the Previous Scores names)
TRACE(820763D0)
TRACE(820765A8)
TRACE(820770F8)
// (820762F0 too: the friends board's scores)
TRACE(820764C0)

#endif
