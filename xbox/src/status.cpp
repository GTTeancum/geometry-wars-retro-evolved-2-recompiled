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
volatile u64 gw_part_cycles[kPartCount];

extern "C" {
// Read by xbox/tools/read_log.py: write position, then the text.
volatile u32 gw_log_pos = 0;
char gw_log_ring[65536];
}

namespace {

CRITICAL_SECTION g_log_lock;
bool g_log_ready = false;
volatile bool g_fatal = false;
u32 g_drawn_per_second;  // frames shown in the last second (status_thread)
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

// On hardware there is no monitor to read the ring: every 5 s it goes to
// D:\gw2log.txt next to the game (fails quietly on a read-only disc), with
// the counters and the frame rate first.
void write_log_file(u32 fps) {
  static char text[sizeof(gw_log_ring) + 256];
  EnterCriticalSection(&g_log_lock);
  int n = snprintf(text, 256, "frames %u (%u/s) drawn %u (%u/s) interrupts %u gpu_packets %u draws %u guest %u KiB%s%s\n",
                   gw_stats.swaps, fps, gw_stats.drawn, g_drawn_per_second, gw_stats.interrupts, gw_stats.gpu_packets, gw_stats.gpu_draws,
                   gw_memory_committed() >> 10, g_fatal ? " FATAL: " : "", g_fatal ? g_fatal_text : "");
  if (n < 0 || n > 255) n = 255;
  const u32 end = gw_log_pos, start = end > sizeof(gw_log_ring) ? end - sizeof(gw_log_ring) : 0;
  for (u32 p = start; p < end; ++p) text[n++] = gw_log_ring[p & (sizeof(gw_log_ring) - 1)];
  LeaveCriticalSection(&g_log_lock);
  HANDLE f = CreateFileA("D:\\gw2log.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD written;
  WriteFile(f, text, (DWORD)n, &written, nullptr);
  CloseHandle(f);
}

DWORD WINAPI status_thread(LPVOID) {
  static char tail[2048];
  u32 seconds = 0;
  u32 last_swaps = 0;
  DWORD last_tick = GetTickCount();
  u32 fps = 0;
  u32 stalled_seconds = 0;
  for (;;) {
    Sleep(1000);
    DWORD now = GetTickCount();
    u32 swaps = gw_stats.swaps;
    fps = (swaps - last_swaps) * 1000 / (now - last_tick ? now - last_tick : 1);
    if (gw_nv2a_active) gw_crtc_pitch();
    static u32 last_drawn;
    const u32 drawn = gw_stats.drawn;
    g_drawn_per_second = (drawn - last_drawn) * 1000 / (now - last_tick ? now - last_tick : 1);
    last_drawn = drawn;
    if (swaps == last_swaps && ++stalled_seconds % 10 == 0) {
      gw_log("no frame for %u s; renderer stage %u; threads:\n", stalled_seconds, gw_nv2a_stage);
      gw_dump_threads();
    }
    if (swaps != last_swaps) stalled_seconds = 0;
    last_swaps = swaps;
    last_tick = now;
    if (++seconds % 5 == 0) {
      // ms of CPU per frame for each timed part, at 733 MHz.
      static u64 last_cycles[kPartCount];
      static u32 last_frames, last_kicks;
      const u32 frames = swaps - last_frames;
      if (frames) {
        char text[192];
        const u32 kicks = gw_gpu_kicks;
        int n = snprintf(text, sizeof(text), "cpu ms/frame over %u frames (%u kicks a frame):", frames,
                         (kicks - last_kicks) / frames);
        last_kicks = kicks;
        static const char* const kNames[kPartCount] = {"grid", "particles", "stars", "commands", "audio"};
        for (int i = 0; i < kPartCount && n < (int)sizeof(text) - 24; ++i) {
          const u64 c = gw_part_cycles[i];
          // tenths of a ms (this printf has no %f)
          const u32 tenths = (u32)((c - last_cycles[i]) / 73300 / frames);
          n += snprintf(text + n, sizeof(text) - n, " %s %u.%u", kNames[i], tenths / 10, tenths % 10);
          last_cycles[i] = c;
        }
        gw_log("%s\n", text);
      }
      last_frames = swaps;
      write_log_file(fps);
    }
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
    if (gw_nv2a_active) continue;  // pbkit owns the screen
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
  // The log as it was at each of the first three fatal errors, in
  // D:\gw2crash1.txt ..3: the ring is soon filled by what follows.
  static volatile long saved = 0;
  const long number = InterlockedIncrement(&saved);
  if (number <= 3) {
    static char text[sizeof(gw_log_ring)];
    if (g_log_ready) EnterCriticalSection(&g_log_lock);
    const u32 end = gw_log_pos, start = end > sizeof(gw_log_ring) ? end - sizeof(gw_log_ring) : 0;
    u32 n = 0;
    for (u32 p = start; p < end; ++p) text[n++] = gw_log_ring[p & (sizeof(gw_log_ring) - 1)];
    if (g_log_ready) LeaveCriticalSection(&g_log_lock);
    char name[32];
    snprintf(name, sizeof(name), "D:\\gw2crash%ld.txt", number);
    HANDLE f = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD written;
      WriteFile(f, text, n, &written, nullptr);
      CloseHandle(f);
    }
  }
  for (;;) Sleep(1000);
}

void gw_status_init() {
  InitializeCriticalSection(&g_log_lock);
  g_log_ready = true;
  // 16-bit: pbkit sizes its back buffers from this mode (memory is tight).
  // make VIDEO=720p asks for 1280x720 (the game's own resolution; needs a
  // component or HDMI connection with 720p enabled in the console settings)
  // and falls back to 640x480. It costs about 6 MB more than 640x480: three
  // 1280x720 colour buffers and pbkit's 32-bit depth buffer.
  // The first mode the console's video settings allow: 720p (on a console
  // set to 720p a 640x480 request may not take, and the display then scans
  // the 640-wide frames at another width), then 480p widescreen, then
  // 640x480. make VIDEO=480 asks for 640x480 only.
#ifdef GW_VIDEO_480
  static const int kModes[][2] = {{640, 480}};
#else
  static const int kModes[][2] = {{1280, 720}, {720, 480}, {640, 480}};
#endif
  for (const auto& m : kModes) {
    const bool ok = XVideoSetMode(m[0], m[1], 16, REFRESH_DEFAULT);
    gw_log("video: %dx%d %s\n", m[0], m[1], ok ? "set" : "not available");
    if (ok) break;
  }
  const VIDEO_MODE mode = XVideoGetMode();
  gw_log("video: in use %dx%d, %d bpp, %d Hz\n", mode.width, mode.height, mode.bpp, mode.refresh);
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
