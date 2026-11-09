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
    // The display's mode registers, logged when they change: after hours the
    // console was found scanning 640x480 timing while the game drew 720p
    // (a black screen on the TV). Memory-mapped (no index port), read only.
    if (gw_nv2a_active) {
      static const u32 kRegs[] = {0xFD680800, 0xFD680804, 0xFD680808, 0xFD680820, 0xFD680824,
                                  0xFD680828, 0xFD680600, 0xFD600804};
      static u32 seen[sizeof(kRegs) / 4];
      static bool have = false;
      bool changed = false;
      for (u32 i = 0; i < sizeof(kRegs) / 4; ++i) {
        const u32 v = *(volatile u32*)kRegs[i];
        if (!have) seen[i] = v;
        else if (v != seen[i]) {
          gw_log("display: %08X changed %08X -> %08X at %u s (frame %u)\n", kRegs[i], seen[i], v, seconds,
                 gw_stats.swaps);
          changed = true;
        }
      }
      // Changed under the game (the console found scanning 640x480 timing
      // after hours, the game's 720p frames black on the TV): the mode set
      // again (at most every 10 s), then the registers read afresh.
      static u32 last_fix;
      if (changed && seconds - last_fix >= 10) {
        last_fix = seconds;
        gw_video_set();
        gw_crtc_pitch();
        for (u32 i = 0; i < sizeof(kRegs) / 4; ++i) seen[i] = *(volatile u32*)kRegs[i];
      } else if (changed) {
        for (u32 i = 0; i < sizeof(kRegs) / 4; ++i) seen[i] = *(volatile u32*)kRegs[i];
      }
      if (!have) {
        gw_log("display: FP %08X %08X %08X / %08X %08X %08X, general %08X, crtc config %08X\n", seen[0], seen[1],
               seen[2], seen[3], seen[4], seen[5], seen[6], seen[7]);
        have = true;
      }
    }
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
#ifdef GW_PARTICLE_STRESS
    if (seconds % 5 == 4) gw_log_memory("every 5 s (particle stress)");
#else
    if (seconds % 300 == 299) gw_log_memory("every 5 minutes");
#endif  // (a soak's memory over time)
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

// The video mode: the highest the console's video settings allow of 720p,
// 480p and 480i (16-bit: pbkit sizes its back buffers from this mode, and
// memory is tight). 1080i is left out: its frame buffers would take about
// 14 MB more than 720p's. On a 4:3 setting at 480 lines the 16:9 picture
// is letterboxed (gw_video_letterbox, used by src/nv2a.cpp). make VIDEO=480
// asks for 640x480 only. Also called again if the display's mode changes
// under the game (status_thread).
namespace {
int g_video_w, g_video_h;
}  // namespace

bool gw_video_letterbox() { return g_video_h == 480 && !(XVideoGetEncoderSettings() & VIDEO_WIDESCREEN); }

void gw_video_set() {
  const DWORD settings = XVideoGetEncoderSettings();
  if (g_video_w) {  // again: the mode chosen at start
    const bool ok = XVideoSetMode(g_video_w, g_video_h, 16, REFRESH_DEFAULT);
    gw_log("video: %dx%d set again %s\n", g_video_w, g_video_h, ok ? "" : "(failed)");
    return;
  }
  int modes[3][2];
  int n = 0;
#ifndef GW_VIDEO_480
  if (settings & VIDEO_MODE_720P) modes[n][0] = 1280, modes[n++][1] = 720;
  if (settings & VIDEO_MODE_480P) modes[n][0] = 720, modes[n++][1] = 480;
#endif
  modes[n][0] = 640, modes[n++][1] = 480;
  gw_log("video: console settings %08X (720p %s, 1080i %s, 480p %s, %s)\n", (u32)settings,
         settings & VIDEO_MODE_720P ? "on" : "off", settings & VIDEO_MODE_1080I ? "on" : "off",
         settings & VIDEO_MODE_480P ? "on" : "off", settings & VIDEO_WIDESCREEN ? "widescreen" : "4:3");
  for (int i = 0; i < n; ++i) {
    const bool ok = XVideoSetMode(modes[i][0], modes[i][1], 16, REFRESH_DEFAULT);
    gw_log("video: %dx%d %s\n", modes[i][0], modes[i][1], ok ? "set" : "not available");
    if (ok) {
      g_video_w = modes[i][0];
      g_video_h = modes[i][1];
      break;
    }
  }
  if (gw_video_letterbox()) gw_log("video: 4:3, the picture letterboxed\n");
}

void gw_status_init() {
  InitializeCriticalSection(&g_log_lock);
  g_log_ready = true;
  gw_video_set();
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
