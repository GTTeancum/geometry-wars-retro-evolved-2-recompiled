// Log ring buffer and the on-screen status page.
//
// gw_log_ring holds the most recent log text; xbox/tools/read_log.py reads it
// from a running XEMU through the monitor (address from the link map). The
// status page redraws once a second with the counters and the last log lines.
#include "runtime.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <stdio.h>
#include <windows.h>

GwStats gw_stats;

extern "C" {
// Read by xbox/tools/read_log.py: write position, then the text.
volatile u32 gw_log_pos = 0;
char gw_log_ring[65536];
}

namespace {

CRITICAL_SECTION g_log_lock;
bool g_log_ready = false;
volatile bool g_fatal = false;
char g_fatal_text[256];

void ring_append(const char* text) {
  for (const char* p = text; *p; ++p) {
    gw_log_ring[gw_log_pos & (sizeof(gw_log_ring) - 1)] = *p;
    ++gw_log_pos;
  }
}

void tail_lines(char* out, size_t out_size, int lines) {
  u32 end = gw_log_pos;
  u32 start = end > sizeof(gw_log_ring) ? end - sizeof(gw_log_ring) : 0;
  u32 p = end;
  int found = 0;
  while (p > start) {
    if (gw_log_ring[(p - 1) & (sizeof(gw_log_ring) - 1)] == '\n' && p != end && ++found >= lines) break;
    --p;
  }
  size_t n = 0;
  for (; p < end && n + 1 < out_size; ++p) out[n++] = gw_log_ring[p & (sizeof(gw_log_ring) - 1)];
  out[n] = 0;
}

DWORD WINAPI status_thread(LPVOID) {
  static char tail[2048];
  u32 last_swaps = 0;
  DWORD last_tick = GetTickCount();
  u32 fps = 0;
  u32 stalled_seconds = 0;
  for (;;) {
    Sleep(1000);
    DWORD now = GetTickCount();
    u32 swaps = gw_stats.swaps;
    fps = (swaps - last_swaps) * 1000 / (now - last_tick ? now - last_tick : 1);
    if (swaps == last_swaps && ++stalled_seconds % 10 == 0) {
      gw_log("no frame for %u s; threads:\n", stalled_seconds);
      gw_dump_threads();
    }
    if (swaps != last_swaps) stalled_seconds = 0;
    last_swaps = swaps;
    last_tick = now;
    EnterCriticalSection(&g_log_lock);
    tail_lines(tail, sizeof(tail), 10);
    // Clip each line to the screen width so wrapping never scrolls the
    // counters off the top.
    {
      char* out = tail;
      int column = 0;
      for (const char* p = tail; *p; ++p) {
        if (*p == '\n') column = -1;
        if (column < 74) *out++ = *p;
        ++column;
      }
      *out = 0;
    }
    LeaveCriticalSection(&g_log_lock);
    debugClearScreen();
    debugPrint("Geometry Wars: Retro Evolved 2 - Xbox bring-up\n");
    debugPrint("frames %u (%u/s)  interrupts %u  gpu packets %u  draws %u\n", swaps, fps,
               gw_stats.interrupts, gw_stats.gpu_packets, gw_stats.gpu_draws);
    debugPrint("threads %u  files %u  read %u KiB  guest memory %u KiB  unimplemented %u\n",
               gw_stats.threads, gw_stats.files_opened, gw_stats.bytes_read >> 10,
               gw_memory_committed() >> 10, gw_stats.unimplemented);
    if (g_fatal) debugPrint("\nFATAL: %s\n", g_fatal_text);
    debugPrint("\n%s", tail);
  }
  return 0;
}

}  // namespace

void gw_log(const char* fmt, ...) {
  char text[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  if (g_log_ready) EnterCriticalSection(&g_log_lock);
  ring_append(text);
  if (g_log_ready) LeaveCriticalSection(&g_log_lock);
}

void gw_fatal(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_fatal_text, sizeof(g_fatal_text), fmt, args);
  va_end(args);
  gw_log("FATAL: %s\n", g_fatal_text);
  g_fatal = true;
  for (;;) Sleep(1000);
}

void gw_status_init() {
  InitializeCriticalSection(&g_log_lock);
  g_log_ready = true;
  XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
  CreateThread(nullptr, 0, status_thread, nullptr, 0, nullptr);
}

void gw_status_frame() { ++gw_stats.swaps; }

// Guest call chain: each frame's back chain is at 0(r1) and the caller saved
// its LR at -8 from the previous frame's stack pointer (Xbox 360 convention).
void gw_log_backtrace(PPCContext& ctx) {
  char text[256];
  int n = snprintf(text, sizeof(text), "  called from %08X", (u32)ctx.lr);
  u32 sp = ctx.r1.u32;
  for (int i = 0; i < 8 && n < (int)sizeof(text) - 12; ++i) {
    if (sp < kGuestStackBase || sp >= kGuestStackBase + kGuestStackSize) break;
    u32 prev = gw_load32(sp);
    if (prev <= sp || prev >= kGuestStackBase + kGuestStackSize) break;
    n += snprintf(text + n, sizeof(text) - n, " <- %08X", gw_load32(prev - 8));
    sp = prev;
  }
  gw_log("%s\n", text);
}
