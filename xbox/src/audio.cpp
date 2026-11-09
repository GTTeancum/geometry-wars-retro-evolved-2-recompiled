// Sound out: the native mixer's frames (src/sound_native.cpp) played through
// the Xbox's AC97, 48 kHz 16-bit stereo. The game's own XAudio2 does not
// run: its render driver registration only starts this.
//
// A thread keeps kAhead frames of 256 samples (5.33 ms each) queued in the
// AC97's ring of 32 buffers, reading which buffer the AC97 is playing; it
// mixes at most one frame per pass and always pauses after, so it can never
// take the processor from the game.
#include "runtime.h"

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <xmmintrin.h>

// The sound engine's time by part (src/sound_native.cpp), in cycles.
extern unsigned long long g_prof_control, g_prof_audio, g_prof_sound, g_prof_effects, g_prof_reverb,
    g_prof_collect, g_prof_mix, g_prof_mark, g_prof_lock, g_prof_out;

namespace {

constexpr u32 kSamples = 256;              // a frame: 256 samples at 48 kHz (5.33 ms)
constexpr u32 kFrameBytes = kSamples * 4;  // 16-bit stereo
constexpr u32 kBuffers = 32;               // the AC97's buffer descriptors
constexpr u32 kAhead = 12;                 // frames queued ahead (64 ms)
constexpr u32 kBatch = 4;                  // frames mixed together (21 ms)
volatile u8* const kAc97 = (volatile u8*)0xFEC00000;  // its registers: 0x114 the buffer playing

// The AC97's buffer descriptors (analog out, then the digital out, which
// plays the same buffers): address, length in 16-bit words, control (0: no
// interrupt; this driver reads the buffer playing instead). Set up here,
// not with nxdk's XAudioInit: that connects an interrupt handler which
// claims every interrupt on its line.
struct Descriptor {
  u32 address;
  u16 words, control;
};
Descriptor* g_descriptors;  // 2 x kBuffers
u32 g_next;                 // the next descriptor
u8* g_buffers;              // kBuffers frames, contiguous
u32 g_csr_seen;             // the SSE control word found before a mix

bool ac97_init() {
  g_descriptors = (Descriptor*)MmAllocateContiguousMemoryEx(2 * kBuffers * sizeof(Descriptor), 0, 0x03FFAFFF, 0,
                                                            PAGE_READWRITE | PAGE_NOCACHE);
  if (!g_descriptors) return false;
  memset(g_descriptors, 0, 2 * kBuffers * sizeof(Descriptor));
  volatile u32* const mmio = (volatile u32*)kAc97;
  mmio[0x12C / 4] &= ~2u;  // cold reset
  Sleep(1);
  mmio[0x12C / 4] |= 2;
  for (int t = 0; !(mmio[0x130 / 4] & 0x100); ++t) {  // codec ready
    if (t > 1000) return false;
    Sleep(1);
  }
  kAc97[0x11B] = 0x1E;  // reset the analog bus master
  for (int t = 0; kAc97[0x11B] & 2; ++t) {
    if (t > 1000) return false;
    Sleep(1);
  }
  kAc97[0x17B] = 0x1E;  // and the digital one
  for (int t = 0; kAc97[0x17B] & 2; ++t) {
    if (t > 1000) return false;
    Sleep(1);
  }
  kAc97[0x116] = 0xFF;  // clear status
  kAc97[0x176] = 0xFF;
  mmio[0x100 / 4] = 0;  // no input
  mmio[0x110 / 4] = MmGetPhysicalAddress(g_descriptors);
  mmio[0x170 / 4] = MmGetPhysicalAddress(g_descriptors + kBuffers);
  kAc97[0x11B] = 0;  // stopped, no interrupts
  kAc97[0x17B] = 0;
  return true;
}

void ac97_queue(u8* buffer, u32 bytes) {
  const u32 address = MmGetPhysicalAddress(buffer);
  for (u32 out = 0; out < 2; ++out) g_descriptors[out * kBuffers + g_next] = {address, (u16)(bytes / 2), 0};
  kAc97[0x115] = (u8)g_next;  // last valid descriptor
  kAc97[0x175] = (u8)g_next;
  g_next = (g_next + 1) % kBuffers;
}

}  // namespace

// The AC97's two bus masters stopped and reset, so nothing is still playing
// out of this program's buffers when the console reboots to the dashboard.
void gw_audio_stop() {
  kAc97[0x11B] = 0;
  kAc97[0x17B] = 0;
  kAc97[0x11B] = 2;
  kAc97[0x17B] = 2;
  for (int t = 0; (kAc97[0x11B] & 2) && t < 100000; ++t) {
  }
  for (int t = 0; (kAc97[0x17B] & 2) && t < 100000; ++t) {
  }
}

namespace {

DWORD WINAPI audio_thread(LPVOID) {
  gw_log("audio: host thread %u\n", (u32)GetCurrentThreadId());
  // Results too small for a normal float flushed to zero: the sound graphs'
  // filters decay into that range, where the Pentium III takes hundreds of
  // cycles per operation. (Flush-to-zero only: the P3 has no
  // denormals-are-zero bit, and setting it would fault.)
  _mm_setcsr(_mm_getcsr() | 0x8000);
  g_buffers = (u8*)MmAllocateContiguousMemoryEx(kBuffers * kFrameBytes, 0, 0x03FFAFFF, 0,
                                                PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (!g_buffers || !ac97_init()) {
    gw_log("audio: the AC97 did not come up\n");
    return 0;
  }
  memset(g_buffers, 0, kBuffers * kFrameBytes);
  u32 queued = 0;  // frames handed to the AC97
  u64 mix_cycles = 0, energy = 0;
  u32 mixes = 0;
  static i16 frame[kSamples * 2];
  auto provide = [&]() {
    const u64 t0 = __builtin_ia32_rdtsc();
    {
      GwPartTimer timer(kPartAudio);
      // Flush-to-zero for the mix only (set here each time: the thread's
      // setting does not survive), the caller's setting restored after.
      const u32 csr = _mm_getcsr();
      g_csr_seen = csr;
      _mm_setcsr(csr | 0x8000);
      gw_sound_mix(frame, kSamples);
      _mm_setcsr(csr);
    }
    mix_cycles += __builtin_ia32_rdtsc() - t0;
    ++mixes;
    for (u32 i = 0; i < kSamples * 2; i += 8) energy += (u64)((i32)frame[i] * frame[i]);
    u8* buffer = g_buffers + (queued % kBuffers) * kFrameBytes;
    memcpy(buffer, frame, kFrameBytes);
    ac97_queue(buffer, kFrameBytes);
    ++queued;
  };
  for (u32 i = 0; i < kAhead; ++i) provide();
  kAc97[0x11B] = 1;  // run, no interrupts
  kAc97[0x17B] = 1;
  for (;;) {
    // Frames queued and not yet played: from the buffer playing to the next
    // one to fill.
    const u32 playing = kAc97[0x114] & 31;
    const u32 ahead = (queued % kBuffers - playing + kBuffers) % kBuffers;
    static DWORD last = 0;
    if (GetTickCount() - last > 5000) {
      last = GetTickCount();
      // Mix cost per frame (a frame lasts 5333 us) and the output's level
      // (root mean square of the samples, 0..32767).
      const u32 rms = mixes ? (u32)__builtin_sqrtf((float)(energy / ((u64)mixes * kSamples / 4))) : 0;
      gw_log("audio: %u frames out, %u ahead, mix %u us a frame, level %u (SSE control %04X)\n", queued, ahead,
             mixes ? (u32)(mix_cycles / mixes / 733) : 0, rms, g_csr_seen);
      if (mixes) {
        gw_log("audio: us a frame: control %u, mixing %u (sounds %u, effects %u), reverb %u\n",
               (u32)(g_prof_control / mixes / 733), (u32)(g_prof_audio / mixes / 733),
               (u32)(g_prof_sound / mixes / 733), (u32)(g_prof_effects / mixes / 733),
               (u32)(g_prof_reverb / mixes / 733));
        gw_log("audio: us a frame: combining %u, mixing loops %u, marking %u, lock %u, output %u\n",
               (u32)(g_prof_collect / mixes / 733), (u32)(g_prof_mix / mixes / 733),
               (u32)(g_prof_mark / mixes / 733), (u32)(g_prof_lock / mixes / 733),
               (u32)(g_prof_out / mixes / 733));
        g_prof_control = g_prof_audio = g_prof_sound = g_prof_effects = g_prof_reverb = 0;
        g_prof_collect = g_prof_mix = g_prof_mark = g_prof_lock = g_prof_out = 0;
      }
      mix_cycles = energy = 0;
      mixes = 0;
    }
    // Frames are mixed kBatch at a time, back to back: between pauses the
    // game's threads run and push the engine's data out of the processor's
    // cache, so the first frame of a batch pays for fetching it and the
    // others find it there. Then a pause (Sleep(1) can last several ms).
    if (ahead + kBatch <= kAhead)
      for (u32 a = ahead; a < kAhead; ++a) provide();
    Sleep(1);
  }
  return 0;
}

}  // namespace

// Started by the native sound engine's start-up (gw_sound_start, from the
// game's audio initialisation), after the guest's memory windows are
// reserved: its contiguous allocations must not land in them.
void gw_audio_start() {
  static bool started = false;
  if (started) return;
  started = true;
  // Above the game's threads, so it runs as soon as its pause ends; it mixes
  // only the frames missing and pauses again, so it cannot hold the processor.
  HANDLE t = CreateThread(nullptr, 64 * 1024, audio_thread, nullptr, 0, nullptr);
  if (t) SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
}

// The 360's audio render driver: the game's XAudio2 never starts now (its
// sound layer is native, src/sound_game.cpp), so these are not reached.
void gw_audio_register(u32, u32) {}
void gw_audio_unregister() {}
void gw_audio_frame(u32) {}
