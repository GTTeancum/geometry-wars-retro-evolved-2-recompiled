// PC check of the native sound engine (src/sound_native.cpp): plays the
// game's sound graphs from the staged disc (xbox/build/staged-disc/Audio)
// through a scripted sequence of the game's actions and parameters, and
// writes what comes out as WAV files with the loudness of each second.
//   sound_check.exe [out_dir]
// Writes menu.wav (the menu music), game.wav (Song 1 with shots and
// explosions) and song3.wav. Built by tools/sound_check.cmd.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "runtime.h"

void gw_log(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vprintf(fmt, a);
  va_end(a);
}
void gw_audio_start() {}
extern unsigned long long g_prof_control, g_prof_audio, g_prof_sound, g_prof_decode, g_prof_effects;

namespace {
struct Wav {
  FILE* f = nullptr;
  u32 frames = 0;
  double sum = 0, peak = 0, cost = 0;
  u32 second_frames = 0;
  void open(const char* path) {
    f = fopen(path, "wb");
    u8 h[44] = {};
    fwrite(h, 1, 44, f);
  }
  void close() {
    const u32 bytes = frames * 4;
    u8 h[44];
    auto put32 = [&](int at, u32 v) { memcpy(h + at, &v, 4); };
    auto put16 = [&](int at, u16 v) { memcpy(h + at, &v, 2); };
    memcpy(h, "RIFF", 4);
    put32(4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, 48000);
    put32(28, 48000 * 4);
    put16(32, 4);
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, bytes);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fclose(f);
  }
};

LARGE_INTEGER g_freq;

// n seconds of sound; 'each' is called before every 256-sample pass with the
// pass number.
template <typename F>
void render(Wav& w, double seconds, F each) {
  const u32 passes = (u32)(seconds * 48000 / 256);
  static i16 buf[512];
  for (u32 p = 0; p < passes; ++p) {
    each(p);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    gw_sound_mix(buf, 256);
    QueryPerformanceCounter(&b);
    w.cost += (double)(b.QuadPart - a.QuadPart) / (double)g_freq.QuadPart;
    fwrite(buf, 4, 256, w.f);
    for (u32 i = 0; i < 512; ++i) {
      const double v = buf[i] / 32768.0;
      w.sum += v * v;
      if (fabs(v) > w.peak) w.peak = fabs(v);
    }
    w.frames += 256;
    w.second_frames += 256;
    if (w.second_frames >= 48000) {
      printf("  %5.1f s  level %.4f  peak %.3f  cost %.0f us/pass\n", w.frames / 48000.0,
             sqrt(w.sum / (w.second_frames * 2)), w.peak, w.cost / (w.second_frames / 256) * 1e6);
      printf("        Mcycles: control %.1f audio %.1f (sounds %.1f, effects %.1f)\n", g_prof_control / 1e6, g_prof_audio / 1e6,
             g_prof_sound / 1e6, g_prof_effects / 1e6);
      g_prof_control = g_prof_audio = g_prof_sound = g_prof_decode = g_prof_effects = 0;
      if (getenv("SOUND_DEBUG") && (w.frames / 48000) % 5 == 2) gw_sound_debug();
      w.sum = w.peak = w.cost = 0;
      w.second_frames = 0;
    }
    // Real time (as on the Xbox, so the streams keep up as they would there);
    // NO_SLEEP: as fast as it goes (for timing the mix).
    if (!getenv("NO_SLEEP")) {
      static LARGE_INTEGER t0;
      if (!t0.QuadPart) QueryPerformanceCounter(&t0);
      static u64 passes = 0;
      ++passes;
      for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - t0.QuadPart) / (double)g_freq.QuadPart >= passes * 256.0 / 48000.0) break;
        Sleep(1);
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : ".";
  QueryPerformanceFrequency(&g_freq);
  gw_sound_start();
  gw_sound_set_param("SFX_Volume", 1.0f);
  gw_sound_set_param("Music_Volume", 1.0f);
  gw_sound_action("Silence");
  gw_sound_action("Game_start");
  gw_sound_action("Start_UI_Music");
  Sleep(1500);  // the bank in memory before the first pass (a repeatable start)
  char path[MAX_PATH];
  {
    Wav w;
    snprintf(path, sizeof(path), "%s\\menu.wav", out);
    w.open(path);
    printf("menu:\n");
    render(w, 20, [](u32 p) {
      if (p % 375 == 100) gw_sound_action("Bong");
    });
    w.close();
  }
  {
    Wav w;
    snprintf(path, sizeof(path), "%s\\game.wav", out);
    w.open(path);
    printf("song 1 in a game:\n");
    gw_sound_action("Start_InGame_Music");
    gw_sound_action("StartSong1");
    gw_sound_set_param("SongID", 1.0f);
    Sleep(1500);
    gw_sound_action("Player_spawn");
    render(w, 30, [](u32 p) {
      gw_sound_set_param("Thrust", (p / 200) % 2 ? 1.0f : 0.0f);
      gw_sound_set_param("Multiplier", (float)(1 + p / 500));
      gw_sound_set_param("Score", (float)(p * 50));
      if (p % 19 == 0) gw_sound_action("Fire_normal");
      if (p % 90 == 45) gw_sound_action("Enemy_explode");
      if (p % 150 == 10) gw_sound_action("Grunt_spawn");
      if (p == 3000) gw_sound_action("Fire_smartbomb");
    });
    w.close();
  }
  {
    Wav w;
    snprintf(path, sizeof(path), "%s\\song3.wav", out);
    w.open(path);
    printf("song 3:\n");
    gw_sound_action("Stop_InGame_Music");
    gw_sound_action("Start_InGame_Music");
    gw_sound_action("StartSong3");
    gw_sound_set_param("SongID", 3.0f);
    Sleep(1500);
    render(w, 30, [](u32 p) {
      if (p % 23 == 0) gw_sound_action("Fire_normal");
      if (p % 70 == 30) gw_sound_action("Enemy_explode");
    });
    w.close();
  }
  return 0;
}
