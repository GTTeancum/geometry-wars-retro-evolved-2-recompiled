// Native sound engine: the game's sound-design graphs (audio\*.dat, "AOBJ")
// played by our own interpreter, with the sounds from the converted banks.
// Nothing of the game's own audio engine, its XAudio2 or the 360's XMA
// decoder runs (src/sound_game.cpp cuts the game over to this).
//
// The specification this follows (format, every component type's
// semantics, the engine's timing) was derived from the game's code; the
// guest functions are named in it. In short:
//   - one graph per file, one instance each, created in the game's order:
//     System, FX, UIMusic, Song_1..6; the others' 5.1 outputs (and the music
//     objects' mono reverb sends) feed System's inputs one pass later;
//   - one audio pass = 256 samples at 48 kHz; every second pass is a control
//     pass: first every Script (index order), then every other stateful
//     component (index order);
//   - values are pulled on demand (math cached once per pass), audio is
//     pulled from the output ports through mixers to the sound players;
//   - the game fires ActionIns by name (which start Scripts) and sets
//     ParameterIns by name; it reads one ParameterOut (TransitionSwitch).
// System's output (L R C LFE Ls Rs) is played as stereo on the AC97, with
// its reverb send through a reverb of our own (the 360's was XAudio's
// built-in I3DL2 reverb, not part of the game).
//
// Sounds: <bank>.adpcm next to each bank (xbox/tools/convert_banks.py):
// 4-bit IMA ADPCM, Microsoft block layout. Waves are found by the names in
// the game's .baf (its WAVE records; the converted entries are in the same
// order). The effects' bank is held in memory; the music streams from the
// disc (the first blocks of each music wave held in memory so it starts at
// once). A sound mixed in at near silence (under kSilentGain, all the way
// to the output) is skipped undecoded, keeping its place: songs keep dozens
// of layers running and switch sections by volume.
//
// The surround mix is carried as stereo (see mixer_shape), and chains of
// mixers are combined so each sound is mixed once (see collect).
#include "runtime.h"

#ifndef GW_SOUND_PROFILE
#include <xboxkrnl/xboxkrnl.h>  // MmQueryStatistics (pcm_cache)
#endif
#ifdef GW_SOUND_APU
#include <xboxkrnl/xboxkrnl.h>

#include "apu.h"
#endif

#include <math.h>
#include <new>
#include <windows.h>
#include <xmmintrin.h>

// nxdk builds with -fno-builtin, which makes fabsf, fmaxf, fminf and sqrtf
// library calls (returning through the x87 stack) even in the inner loops;
// these are the same operations inline (fmaxf / fminf differ only for NaN,
// which the graphs do not produce).
inline float gw_fabsf(float x) {
  u32 v;
  memcpy(&v, &x, 4);
  v &= 0x7FFFFFFFu;
  memcpy(&x, &v, 4);
  return x;
}
inline float gw_fmaxf(float a, float b) { return a > b ? a : b; }
inline float gw_fminf(float a, float b) { return a < b ? a : b; }
inline float gw_sqrtf(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
// The Pentium III has no hardware prefetcher: each new 32-byte line read
// waits for memory (shared with the GPU). The loops over audio buffers ask
// for their lines ahead (all of a 256-sample buffer is 32 lines).
inline void prefetch(const void* p, u32 bytes) {
  const char* c = (const char*)p;
  for (u32 i = 0; i < bytes; i += 32) _mm_prefetch(c + i, _MM_HINT_T0);
}
#define fabsf gw_fabsf
#define fmaxf gw_fmaxf
#define fminf gw_fminf
#define sqrtf gw_sqrtf

#ifndef GW_AUDIO_DIR
#define GW_AUDIO_DIR "D:\\Audio\\"
#endif

// Time spent, in processor cycles, by part (logged with the audio thread's
// report on the Xbox, and by the PC check).
// Passes a voice's stream was not there in time (a gap in that sound),
// counted for the audio thread's report (src/audio.cpp).
u32 gw_sound_late;
u32 gw_sound_voices;  // voices decoded (a pass each), for the audio report
// Why streams were late since the last report (no ring, the ring behind, the
// ring at another place) and the last late wave's name (src/audio.cpp logs them).
u32 gw_sound_late_why[3];
const char* gw_sound_late_wave = "";
u64 gw_sound_wait_cycles;  // the audio thread idle, waiting for the APU's output (not mixing cost)
unsigned long long g_prof_control, g_prof_audio, g_prof_sound, g_prof_decode, g_prof_effects, g_prof_reverb,
    g_prof_collect, g_prof_mix, g_prof_mark, g_prof_lock, g_prof_out;
// The APU's own parts (zero in the software build): reading its output back
// (the wait for it left out), following its voices' positions and filling
// stream rings, writing its voices' routes.
unsigned long long g_prof_hwread, g_prof_hwpos, g_prof_hwapply;
#define PROF_BEGIN(n) const unsigned long long prof_##n = __builtin_ia32_rdtsc()
#define PROF_END(n) g_prof_##n += __builtin_ia32_rdtsc() - prof_##n

namespace {

// --- constants -------------------------------------------------------------------------
constexpr u32 kPass = 256;
constexpr float kDt = 512.0f / 48000.0f;  // control period (divisor 2)
constexpr u32 kDivisor = 2;
constexpr float kSilentGain = 0.0011f;     // layers below this skip decoding (-59 dB)
constexpr u32 kMaxChannels = 6;
constexpr u32 kMaxInputs = 64;           // a mixer's inputs (the most in the data: 58)
const float kPi = 3.1415927f;

inline u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
inline i32 bes(const u8* p) { return (i32)be32(p); }
inline float bef(const u8* p) {
  const u32 v = be32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline float sqrtz(float x) { return x > 0 ? sqrtf(x) : 0.0f; }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

// --- banks ----------------------------------------------------------------------------
struct Wave {
  u32 original_offset, original_size, rate, channels, samples, block, offset, size;
  char name[32];
  // A streamed wave's first blocks, held in memory (filled by the reader
  // thread at start-up) so it starts at once, before its stream arrives.
  u8* volatile head;
  u32 head_blocks;
  u8* data;  // the whole wave, when held in memory (bank_resident)
  // Decoded (pcm_cache): the samples as the decoder makes them, channels
  // interleaved, and the pass it was last played in.
  mutable i16* pcm;
  mutable u32 pcm_used;
  // The APU's copy (GW_SOUND_APU): each channel as mono Xbox ADPCM at
  // sample-space offset apu_ba[c], apu_samples long, then a silent tail.
  u32 apu_ba[2];
  u32 apu_samples;
  // Xbox ADPCM (a 'GWX1' bank): 36-byte blocks per channel of 64 samples (a
  // header sample, then 63 codes; the 64th code is not played), interleaved
  // per block for stereo; 'block' is a group of them (one stream slot).
  bool xa;
};
struct Bank {
  char name[24];
  HANDLE file;
  u8* data;  // the whole .adpcm when held in memory
  u32 count;
  Wave* waves;
  volatile bool resident;
};
constexpr u32 kBanks = 8;
Bank g_banks[kBanks];
CRITICAL_SECTION g_lock;
CRITICAL_SECTION g_file_lock;  // a bank file's position and read
bool g_ready;

// The engine's own memory (graphs, their state and buffers, the stream
// rings), handed out from 256 KiB blocks and never given back: it lives as
// long as the game. One block for many small pieces instead of one
// allocation each (each of which costs far more than its size). Zeroed.
constexpr u32 kArenaBlock = 256 * 1024;
u8* g_arena;
u32 g_arena_left;
u32 g_arena_total;  // bytes taken from the system
bool g_arena_failed;
CRITICAL_SECTION g_arena_lock;  // the start-up and the reader thread both take from it
void* arena_(u32 size);
void* arena(u32 size) {
  EnterCriticalSection(&g_arena_lock);
  void* p = arena_(size);
  LeaveCriticalSection(&g_arena_lock);
  return p;
}
void* arena_(u32 size) {
  size = (size + 15) & ~15u;
  if (size > kArenaBlock / 4) {  // a big piece: its own allocation
    void* p = calloc(1, size);
    if (p) g_arena_total += size;
    else g_arena_failed = true;
    if (!p) gw_log("sound: no memory for %u bytes\n", size);
    return p;
  }
  if (size > g_arena_left) {
    g_arena = (u8*)calloc(1, kArenaBlock);
    if (!g_arena) {
      g_arena_left = 0;
      g_arena_failed = true;
      gw_log("sound: no memory for %u bytes\n", size);
      return nullptr;
    }
    g_arena_left = kArenaBlock;
    g_arena_total += kArenaBlock;
  }
  void* p = g_arena;
  g_arena += size;
  g_arena_left -= size;
  return p;
}
template <typename T>
T* arena_new(u32 n) {
  T* p = (T*)arena(n * (u32)sizeof(T));
  if (p)
    for (u32 i = 0; i < n; ++i) new (&p[i]) T();
  return p;
}
u32 g_pass;   // audio pass counter
u32 g_epoch;  // control pass counter
volatile bool g_paused;  // the game paused: the songs' graphs stop where they are (as the original)

const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// One ADPCM block -> samples (channels interleaved); returns samples per channel.
// Each (step index, nibble) pair's difference and next step index, worked
// out once from the tables above (the same arithmetic as the standard
// decoder, so the samples are identical), so a sample is a lookup, an add
// and a clamp.
i32 g_adpcm_diff[89 * 16];
u8 g_adpcm_next[89 * 16];
void adpcm_tables() {
  for (int i = 0; i < 89; ++i) {
    for (int nibble = 0; nibble < 16; ++nibble) {
      const int step = kStepTable[i];
      int diff = step >> 3;
      if (nibble & 1) diff += step >> 2;
      if (nibble & 2) diff += step >> 1;
      if (nibble & 4) diff += step;
      g_adpcm_diff[i * 16 + nibble] = nibble & 8 ? -diff : diff;
      const int next = i + kIndexTable[nibble];
      g_adpcm_next[i * 16 + nibble] = (u8)(next < 0 ? 0 : next > 88 ? 88 : next);
    }
  }
}

bool bank_open(const char* name) {
  Bank* slot = nullptr;
  for (Bank& b : g_banks) {
    if (b.count && !_stricmp(b.name, name)) return true;
    if (!b.count && !slot) slot = &b;
  }
  if (!slot) return false;
  char path[96];
  snprintf(path, sizeof(path), GW_AUDIO_DIR "%s.adpcm", name);
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    gw_log("sound: %s missing\n", path);
    return false;
  }
  u32 head[2];
  DWORD done = 0;
  ReadFile(f, head, 8, &done, nullptr);
  // 'GWAD': IMA ADPCM in Microsoft blocks; 'GWX1': Xbox ADPCM (xbox/tools/xadpcm_bank).
  const bool xa = head[0] == 0x31585747;
  if (done != 8 || (head[0] != 0x44415747 && !xa)) {
    CloseHandle(f);
    return false;
  }
  Wave* waves = (Wave*)calloc(head[1], sizeof(Wave));
  for (u32 i = 0; i < head[1]; ++i) {
    ReadFile(f, &waves[i], 32, &done, nullptr);
    // A wave's length is what its blocks hold (one entry, LeaderChange2,
    // claims more: its source decoded shorter than its header said).
    Wave& w = waves[i];
    w.xa = xa;
    if (xa) {
      if (w.channels >= 1 && w.channels <= 2 && w.block >= 36 * w.channels && w.samples > w.size / (36 * w.channels) * 64)
        w.samples = w.size / (36 * w.channels) * 64;
    } else if (w.channels >= 1 && w.channels <= 2 && w.block > 4 * w.channels) {
      const u32 blocks = (w.size + w.block - 1) / w.block;
      const u32 last = w.size - (blocks ? blocks - 1 : 0) * w.block;
      const u32 per = (w.block - 4 * w.channels) * 2 / w.channels + 1;
      const u32 held = blocks && last > 4 * w.channels ? (blocks - 1) * per + (last - 4 * w.channels) * 2 / w.channels + 1 : 0;
      if (w.samples > held) w.samples = held;
    }
  }
  // The names, from the game's bank's WAVE records (same order).
  snprintf(path, sizeof(path), GW_AUDIO_DIR "%s.baf", name);
  HANDLE baf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (baf != INVALID_HANDLE_VALUE) {
    u8 rec[0x60];
    u32 at = 0x34;
    for (u32 i = 0; i < head[1]; ++i) {
      SetFilePointer(baf, at, nullptr, FILE_BEGIN);
      ReadFile(baf, rec, 0x30, &done, nullptr);
      memcpy(waves[i].name, rec + 12, 31);
      at += be32(rec + 4);
    }
    CloseHandle(baf);
  }
  strncpy(slot->name, name, sizeof(slot->name) - 1);
  slot->file = f;
  slot->waves = waves;
  slot->count = head[1];
  gw_log("sound: bank %s, %u waves\n", name, head[1]);
  return true;
}

// Holds a bank's waves of up to `limit` bytes in memory, in one block; the
// others stream (from their first blocks, held by the reader thread).
void bank_resident(Bank& b, u32 limit) {
  u32 total = 0, held = 0;
  for (u32 i = 0; i < b.count; ++i)
    if (b.waves[i].size <= limit) total += b.waves[i].size;
  u8* data = (u8*)malloc(total ? total : 1);
  if (!data) {
    gw_log("sound: no memory for bank %s (%u KiB)\n", b.name, total / 1024);
    return;
  }
  u32 at = 0;
  for (u32 i = 0; i < b.count; ++i) {
    Wave& w = b.waves[i];
    if (w.size > limit) continue;
    DWORD done = 0;
    SetFilePointer(b.file, w.offset, nullptr, FILE_BEGIN);
    ReadFile(b.file, data + at, w.size, &done, nullptr);
    if (done != w.size) continue;  // (left to stream)
    w.data = data + at;
    at += w.size;
    ++held;
  }
  gw_log("sound: bank %s: %u of %u waves in memory (%u KiB)\n", b.name, held, b.count, at / 1024);
#ifndef GW_SOUND_PROFILE
  gw_log_memory("sound bank loaded");
#endif
}

const Wave* find_wave(const char* bank, const char* wave, Bank** out_bank) {
  for (Bank& b : g_banks) {
    if (!b.count || _stricmp(b.name, bank)) continue;
    for (u32 i = 0; i < b.count; ++i) {
      if (!_stricmp(b.waves[i].name, wave)) {
        *out_bank = &b;
        return &b.waves[i];
      }
    }
  }
  return nullptr;
}

// --- a wave being played: decoded as it plays --------------------------------------------
// A playing wave keeps only its decoder's state (each channel's predictor and
// step index, and where it is in the current block): the samples are decoded
// straight into the mix as they are needed, and a jump (a start part way in,
// or coming back after being skipped while silent) decodes from the start of
// its block. Music streams from the disc: each playing streamed wave holds
// one ring of blocks from a fixed pool, which the reader thread fills.
constexpr u32 kMaxBlock = 1024;       // the converted banks' ADPCM block size
constexpr u32 kStreamBlocks = 32;     // a ring: 32 blocks (32 KiB, about 0.35 s of stereo)
constexpr u32 kReadBlocks = 16;       // blocks per disc read (16 KiB)
constexpr u32 kRings = 48;            // streams playing at once

struct Reader;
struct Ring {
  u8* data;               // kStreamBlocks x kMaxBlock
  Reader* owner;          // null: free
  const u32* silent;      // the owner's passes in a row at near silence
};
Ring g_rings[kRings];
bool g_rings_full_logged;

struct Reader {
  Bank* bank = nullptr;
  const Wave* wave = nullptr;
  bool loop = false;
  u32 pos = 0;  // the next sample to play
  // Decoder: block, next sample in it (0 = its header sample), state.
  i32 block = -1;
  u32 at = 0;
  i32 pred[2] = {}, idx[2] = {};
  const u8* bytes = nullptr;
  // Streamed: the ring, its blocks [first, first + written - read) in slots
  // read..written (mod kStreamBlocks); the reader thread asks for 'next'.
  Ring* ring = nullptr;
  u32 ring_read = 0, ring_written = 0, ring_first = 0, ring_next = 0;

  // (A "block" here is what one read takes: an IMA ADPCM block, or a group
  // of Xbox ADPCM blocks.)
  u32 per_block() const {
    return wave->xa ? wave->block / (36 * wave->channels) * 64 : (wave->block - 4 * wave->channels) * 2 / wave->channels + 1;
  }
  u32 blocks() const { return (wave->size + wave->block - 1) / wave->block; }
  u32 block_bytes(u32 b) const { return b + 1 == blocks() ? wave->size - b * wave->block : wave->block; }
  u32 block_samples(u32 b) const {
    return wave->xa ? block_bytes(b) / (36 * wave->channels) * 64
                    : (block_bytes(b) - 4 * wave->channels) * 2 / wave->channels + 1;
  }
  bool streamed() const { return !wave->data; }

  u32 head_blocks() const { return wave->head ? wave->head_blocks : 0; }
  // (g_lock held.) The stream restarted at block b.
  void stream_from(u32 b) {
    ring_read = ring_written = 0;
    ring_first = ring_next = b;
  }
  // (g_lock held.) The ring from block b on (past the blocks held in
  // memory): blocks before it dropped, or the stream restarted there.
  void stream_want(u32 b) {
    if (!ring) return;
    if (b < head_blocks()) b = head_blocks();
    if (ring_first > b) stream_from(b);
    while (ring_read != ring_written && ring_first < b) {
      ++ring_read;
      ++ring_first;
    }
    if (ring_read == ring_written && ring_first != b) stream_from(b);
  }
  // Block b's bytes, or null while a stream has not got there yet.
  const u8* block_data(u32 b) {
    if (!streamed()) return wave->data + b * wave->block;
    stream_want(b);
    if (b < head_blocks()) return wave->head + b * wave->block;
    if (!ring || ring_read == ring_written || ring_first != b) return nullptr;
    return ring->data + (ring_read % kStreamBlocks) * wave->block;
  }
  void start(u32 sample) {
    pos = sample < wave->samples ? sample : 0;
    block = -1;
    if (streamed() && ring) {
      stream_from(0xFFFFFFFF);  // (restarted below, past what is in memory)
      stream_want(pos / per_block());
    }
  }
  // Decodes count samples of the current block, from 'at', into out (every
  // 'stride' floats; null: decode only).
  void decode(u32 c, u32 count, float* out, u32 stride) {
    const u32 wch = wave->channels;
    i32 p = pred[c], ix = idx[c] * 16;
    u32 a = at;
    if (wave->xa) {
      // Xbox ADPCM: each 64 samples a block of its own (its header resets the
      // state), the codes after it as in IMA ADPCM.
      for (; count; --count, ++a) {
        const u32 k = a & 63;
        const u8* blk = bytes + (a >> 6) * 36 * wch;
        if (k == 0) {
          p = (i16)(blk[c * 4] | blk[c * 4 + 1] << 8);
          ix = (blk[c * 4 + 2] > 88 ? 88 : blk[c * 4 + 2]) * 16;
        } else {
          const u32 code = k - 1;
          const u8* q = blk + 4 * wch + (code >> 3) * 4 * wch + 4 * c;
          const int nibble = (q[(code & 7) >> 1] >> ((code & 1) << 2)) & 15;
          p += g_adpcm_diff[ix + nibble];
          p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
          ix = g_adpcm_next[ix + nibble] * 16;
        }
        if (out) {
          *out = (float)p * (1.0f / 32768.0f);
          out += stride;
        }
      }
      pred[c] = p;
      idx[c] = ix / 16;
      return;
    }
    if (a == 0 && count) {
      if (out) {
        *out = (float)p * (1.0f / 32768.0f);
        out += stride;
      }
      ++a;
      --count;
    }
    const u8* q = bytes + 4 * wch + ((a - 1) >> 3) * 4 * wch + 4 * c;
    u32 k = (a - 1) & 7;
    for (; count; --count) {
      const int nibble = (q[k >> 1] >> ((k & 1) << 2)) & 15;
      p += g_adpcm_diff[ix + nibble];
      p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
      ix = g_adpcm_next[ix + nibble] * 16;
      if (out) {
        *out = (float)p * (1.0f / 32768.0f);
        out += stride;
      }
      if (++k == 8) {
        k = 0;
        q += 4 * wch;
      }
    }
    pred[c] = p;
    idx[c] = ix / 16;
  }
  // Both channels of a stereo wave, count samples from 'at', into o0 and o1:
  // the same arithmetic as decode, the two channels' chains interleaved (each
  // sample's step waits on the one before; the Pentium III overlaps two
  // independent chains).
  void decode2(u32 count, float* o0, float* o1) {
    constexpr float kScale = 1.0f / 32768.0f;
    i32 p0 = pred[0], p1 = pred[1], x0 = idx[0] * 16, x1 = idx[1] * 16;
    u32 a = at;
    auto step = [](i32& p, i32& x, int nibble) {
      p += g_adpcm_diff[x + nibble];
      p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
      x = g_adpcm_next[x + nibble] * 16;
    };
    if (wave->xa) {
      for (; count; --count, ++a) {
        const u32 k = a & 63;
        const u8* blk = bytes + (a >> 6) * 72;
        if (k == 0) {
          p0 = (i16)(blk[0] | blk[1] << 8);
          x0 = (blk[2] > 88 ? 88 : blk[2]) * 16;
          p1 = (i16)(blk[4] | blk[5] << 8);
          x1 = (blk[6] > 88 ? 88 : blk[6]) * 16;
        } else {
          const u32 code = k - 1;
          const u8* q = blk + 8 + (code >> 3) * 8 + ((code & 7) >> 1);
          const u32 sh = (code & 1) << 2;
          step(p0, x0, (q[0] >> sh) & 15);
          step(p1, x1, (q[4] >> sh) & 15);
        }
        *o0++ = (float)p0 * kScale;
        *o1++ = (float)p1 * kScale;
      }
    } else {
      if (a == 0 && count) {
        *o0++ = (float)p0 * kScale;
        *o1++ = (float)p1 * kScale;
        ++a;
        --count;
      }
      const u8* q = bytes + 8 + ((a - 1) >> 3) * 8;
      u32 k = (a - 1) & 7;
      for (; count; --count) {
        const u32 sh = (k & 1) << 2;
        step(p0, x0, (q[k >> 1] >> sh) & 15);
        step(p1, x1, (q[4 + (k >> 1)] >> sh) & 15);
        *o0++ = (float)p0 * kScale;
        *o1++ = (float)p1 * kScale;
        if (++k == 8) {
          k = 0;
          q += 8;
        }
      }
    }
    pred[0] = p0;
    pred[1] = p1;
    idx[0] = x0 / 16;
    idx[1] = x1 / 16;
  }
  // The decoder at 'pos'; false while a stream is behind.
  bool sync() {
    const u32 pb = per_block(), b = pos / pb, s = pos - b * pb;
    if (block == (i32)b && at == s && bytes) return true;
    if (block != (i32)b || at > s || !bytes) {
      bytes = block_data(b);
      if (!bytes) {
        block = -1;
        return false;
      }
      block = (i32)b;
      at = 0;
      for (u32 c = 0; c < wave->channels; ++c) {
        pred[c] = (i16)(bytes[c * 4] | bytes[c * 4 + 1] << 8);
        idx[c] = bytes[c * 4 + 2] > 88 ? 88 : bytes[c * 4 + 2];
      }
    }
    for (u32 c = 0; c < wave->channels; ++c) decode(c, s - at, nullptr, 0);
    at = s;
    return true;
  }
  // Up to n samples into out[0..ch) (from 'first' on in each); returns how
  // many; *ended at the end of a wave that does not loop.
  u32 render(float* const* out, u32 ch, u32 first, u32 n, bool* ended) {
    u32 got = 0;
    if (!streamed() && wave->pcm) {
      // Decoded already (pcm_cache): the same samples, converted. The
      // decoder starts again from its block if the cache gives this wave up.
      block = -1;
      bytes = nullptr;
      const u32 wch = wave->channels, both = ch < wch ? ch : wch;
      while (got < n) {
        u32 run = n - got;
        if (run > wave->samples - pos) run = wave->samples - pos;
        if (!run) break;
        const i16* src = wave->pcm + pos * wch;
        for (u32 c = 0; c < both; ++c) {
          float* o = out[c] + first + got;
          for (u32 i = 0; i < run; ++i) o[i] = (float)src[i * wch + c] * (1.0f / 32768.0f);
        }
        for (u32 d = wch; d < ch; ++d) memcpy(out[d] + first + got, out[wch - 1] + first + got, run * 4);
        got += run;
        pos += run;
        if (pos >= wave->samples) {
          if (!loop) {
            *ended = true;
            break;
          }
          pos = 0;
        }
      }
      return got;
    }
    while (got < n) {
      if (!sync()) break;
      u32 run = block_samples((u32)block) - at;
      if (run > n - got) run = n - got;
      if (run > wave->samples - pos) run = wave->samples - pos;
      if (!run) break;
      const u32 wch = wave->channels;
      const u32 at0 = at;
      if (wch == 2 && ch == 2) {
        decode2(run, out[0] + first + got, out[1] + first + got);
        at = at0 + run;
        got += run;
        pos += run;
        if (pos >= wave->samples) {
          if (!loop) {
            *ended = true;
            break;
          }
          pos = 0;
          if (streamed()) stream_want(0);
        }
        continue;
      }
      for (u32 c = 0; c < ch; ++c) {
        if (c < wch) {
          at = at0;
          const i32 p0 = pred[c], i0 = idx[c];
          decode(c, run, out[c] + first + got, 1);
          if (c + 1 < ch && c + 1 >= wch) {  // a mono wave into more channels
            for (u32 d = c + 1; d < ch; ++d) memcpy(out[d] + first + got, out[c] + first + got, run * 4);
          }
          (void)p0;
          (void)i0;
        }
      }
      for (u32 c = ch; c < wch; ++c) {  // channels not wanted: kept in step
        at = at0;
        decode(c, run, nullptr, 0);
      }
      at = at0 + run;
      got += run;
      pos += run;
      if (pos >= wave->samples) {
        if (!loop) {
          *ended = true;
          break;
        }
        pos = 0;
        if (streamed()) stream_want(0);
      }
    }
    return got;
  }
  // Skips n samples undecoded; false at the end of a wave that does not loop.
  bool skip(u32 n) {
    pos += n;
    if (pos >= wave->samples) {
      if (!loop) return false;
      pos %= wave->samples;
    }
    if (streamed()) stream_want(pos / per_block());
    return true;
  }
};

// Decoded sounds: the effects held in memory are decoded once, as they are
// first played, and kept (up to kPcmBudget, the waves played longest ago
// given up first): decoding was about two thirds of the voices' time, and a
// block decodes the same wherever it is started (each block's header sets
// the decoder's state), so the cached samples are the decoder's own. At most
// one wave is decoded a pass (a short wave's decoding is about a millisecond).
constexpr u32 kPcmBudget = 1 << 20;     // bytes
constexpr u32 kPcmWaveMost = 128 * 1024;  // bytes: longer waves are not cached
constexpr u32 kPcmKeepFree = 6 << 20;    // memory left to the game
const Wave* g_pcm_waves[256];
u32 g_pcm_count, g_pcm_bytes, g_pcm_pass = ~0u;

void pcm_cache(const Wave& w) {
  if (w.pcm || !w.data || !w.samples || g_pcm_pass == g_pass) return;
  const u32 bytes = w.samples * w.channels * 2;
  if (bytes > kPcmWaveMost) return;
  g_pcm_pass = g_pass;
  // Room: the waves played longest ago (not this pass) given up.
  while (g_pcm_bytes + bytes > kPcmBudget || g_pcm_count == 256) {
    u32 oldest = ~0u, at = 0;
    for (u32 i = 0; i < g_pcm_count; ++i)
      if (g_pcm_waves[i]->pcm_used < oldest) oldest = g_pcm_waves[i]->pcm_used, at = i;
    if (oldest == ~0u || oldest == g_pass) return;
    const Wave& gone = *g_pcm_waves[at];
    free(gone.pcm);
    gone.pcm = nullptr;
    g_pcm_bytes -= gone.samples * gone.channels * 2;
    g_pcm_waves[at] = g_pcm_waves[--g_pcm_count];
  }
#ifndef GW_SOUND_PROFILE
  MM_STATISTICS stats;
  stats.Length = sizeof(stats);
  MmQueryStatistics(&stats);
  if (stats.AvailablePages * 4096 < kPcmKeepFree + bytes) return;
#endif
  i16* out = (i16*)malloc(bytes);
  if (!out) return;
  // The decoder, block by block (as Reader::sync and render do).
  Reader r;
  r.wave = &w;
  static float tmp[2][4096];
  const u32 wch = w.channels, blocks = r.blocks();
  u32 pos = 0;
  for (u32 b = 0; b < blocks && pos < w.samples; ++b) {
    r.bytes = w.data + b * w.block;
    u32 count = r.block_samples(b);
    if (count > w.samples - pos) count = w.samples - pos;
    if (count > 4096) {  // (no block is this long; refused rather than decoded past tmp)
      free(out);
      return;
    }
    for (u32 c = 0; c < wch; ++c) {
      r.pred[c] = (i16)(r.bytes[c * 4] | r.bytes[c * 4 + 1] << 8);
      r.idx[c] = r.bytes[c * 4 + 2] > 88 ? 88 : r.bytes[c * 4 + 2];
    }
    r.at = 0;
    for (u32 c = 0; c < wch; ++c) r.decode(c, count, tmp[c], 1);
    for (u32 i = 0; i < count; ++i)
      for (u32 c = 0; c < wch; ++c) out[(pos + i) * wch + c] = (i16)(tmp[c][i] * 32768.0f);
    pos += count;
  }
  if (pos < w.samples) {
    free(out);
    return;
  }
  w.pcm = out;
  w.pcm_used = g_pass;
  g_pcm_waves[g_pcm_count++] = &w;
  g_pcm_bytes += bytes;
}

// (g_lock held.) A ring for a playing streamed wave, filled from where it
// is. With none free, the one whose wave has been silent longest is taken
// (that wave keeps its place and streams again when it is heard); with
// every ring's wave being heard, false (the wave stays silent meanwhile).
bool ring_take(Reader& r, const u32* silent) {
  if (!r.streamed() || r.ring) return true;
  Ring* best = nullptr;
  for (Ring& ring : g_rings) {
    if (!ring.data) continue;
    if (!ring.owner) {
      best = &ring;
      break;
    }
    if (*ring.silent > 0 && (!best || *ring.silent > *best->silent)) best = &ring;
  }
  if (!best) {
    if (!g_rings_full_logged) {
      g_rings_full_logged = true;
      gw_log("sound: more than %u streams heard at once; %s left silent\n", kRings, r.wave->name);
    }
    return false;
  }
  if (best->owner) {
    best->owner->ring = nullptr;
    best->owner->bytes = nullptr;
    best->owner->block = -1;
  }
  best->owner = &r;
  best->silent = silent;
  r.ring = best;
  r.block = -1;
  r.stream_from(0xFFFFFFFF);
  r.stream_want(r.pos / r.per_block());
  return true;
}
void ring_give_back(Reader& r) {
  if (!r.ring) return;
  r.ring->owner = nullptr;
  r.ring = nullptr;
  r.bytes = nullptr;
  r.block = -1;
}

// --- graphs ---------------------------------------------------------------------------
struct Graph;
struct Comp;

enum Kind : u8 { kOther, kAudioSound, kAudioMixer, kAudioInput, kAudioOutput };

struct Slot {
  i32 src;
  u32 out;
  float c;
};
inline Slot slot_at(const u8* p) { return {bes(p), be32(p + 4), bef(p + 8)}; }

struct Voice {
  Reader r;
  u32 silent_passes = 0;  // passes in a row mixed at near silence
  bool playing = false, produced = false;
  float carry[2] = {0, 0};
  float fx1[2] = {0, 0}, fy1[2] = {0, 0};  // anti-alias one-pole
  float fa = 0, fb = 1, f_step = -1;
  float target_step = 1, prev_step = 1;
  float gain_target = 0, gain_prev = 0, gain_last = 0;
  float pitch = 1;
  // On the APU (GW_SOUND_APU): a hardware voice per wave channel (-1: none,
  // the voice plays in software), and this pass's mixes into bins.
  // hw: its hardware voice (-1: software; a stereo wave is one stereo
  // voice, its left channel on outputs 0 2 4 6, its right on 1 3 5 7);
  // hw_ring: a streamed wave's ring of Xbox ADPCM blocks the voice loops
  // over (-1: an in-memory wave), with the samples played since the start
  // (hw_played; hw_cbo the last play position seen), the blocks written
  // (hw_out) and the wave's sample it started at (hw_start).
  i16 hw[2] = {-1, -1};
  i16 hw_ring = -1;
  u32 hw_played = 0, hw_cbo = 0, hw_out = 0, hw_start = 0;
  u8 rt_n[2] = {0, 0};
  u8 rt_bin[2][4];
  float rt_gain[2][4];
};

struct Comp {
  u32 type;
  u32 index;  // in its graph
  const u8* b;  // record body (big-endian)
  const char* name;
  Kind kind = kOther;
  // value cache
  u32 stamp = 0xFFFFFFFF;
  float value = 0;
  // generic state
  float s[16] = {};
  u8 flag[4] = {};
  // audio
  u32 channels = 0;
  float (*buf)[kPass] = nullptr;  // channels x 256 (only components that carry audio)
  bool active = false;
  u32 audio_stamp = 0xFFFFFFFF;
  u32 updated = 0xFFFFFFFF;  // the control pass (g_epoch) a mixer or effect last took its settings
  u32 gain_stamp = 0xFFFFFFFF;  // the pass a mixer's gains were last used
  u32 live_stamp = 0xFFFFFFFF;
  bool live = false;     // something under it can sound this pass
  bool inserts = false;  // a mixer with insert effects (its own buffer in the chain)
  bool pulled = false;   // an output port takes it
  // On the APU (GW_SOUND_APU): a sound whose every way out is plain mixing
  // up to bin mixers (its voices can play on the APU), and those mixers; a
  // bin mixer (a graph output's mixer, or one with inserts), its bins while
  // voices mix into it, and how many voices do.
  bool apu_ok = false;
  u8 apu_target_n = 0;
  Comp* apu_targets[4] = {};
  bool apu_out = false;
  i8 apu_bins[4] = {-1, -1, -1, -1};
  u32 apu_refs = 0;
  u32 collected = 0xFFFFFFFF;  // the period its mixes were combined (g_contribs[contrib_first, contrib_end))
  u32 contrib_first = 0, contrib_end = 0;
  // sound players
  Voice* voices = nullptr;
  u32 voice_count = 0;
  i32 last_voice = -1;
  bool resample = true;
  float cached[4] = {-1, -1, -1, -1};  // octave, pitch, dB, volume
  // mixers
  u32 nA = 0, nB = 0, in_ch = 0, out_ch = 0;
  float* current = nullptr;
  float* target = nullptr;
  // input ports: external sums (double-buffered: last pass's)
  float* ext = nullptr;
  float* ext_next = nullptr;
  // input ports: where last pass's sum is (in_now) and where this pass's
  // goes (in_next): the output port's own buffer when one output reaches
  // it (no copy: System runs first in a pass, before the graph that wrote
  // it runs again), else ext_next, which then holds the sum
  const float* in_now[2] = {};
  const float* in_next[2] = {};
  // output ports: destination
  Comp* dest = nullptr;
  // scripts
  u32 pc = 0, sp = 0;
  bool running = false, eq = false, gt = false, lt = false;
  float stack[16];
  // DSP effects
  float d[32] = {};
  float* delay = nullptr;
  u32 delay_blocks = 0;
};

struct Graph {
  char name[24];
  u8* file = nullptr;
  u32 count = 0;
  Comp* c = nullptr;
  const char* strings = nullptr;
  bool live = false;
  // Its sound players and output ports (index lists), to tell quickly
  // whether anything in it plays: a graph with nothing playing (a song not
  // started) costs no audio work.
  u32* sounds = nullptr;
  u32 sound_count = 0;
  u32* outputs = nullptr;
  u32 output_count = 0;
  // Compact per-component state, so a pass touches only what plays (the
  // Xbox's cache is small: visiting every component costs far more than the
  // sound work itself).
  u32* scripts = nullptr;  // Scripts
  u32 script_count = 0;
  u32* ctl = nullptr;      // what a control pass updates (sounds only while they play)
  u32 ctl_count = 0;
  u8* playing = nullptr;   // per component: a sound with a voice playing
  u32 playing_count = 0;
  u32* live_pass = nullptr;  // per component: the pass something under it last played
  u32* parent_first = nullptr;  // per component: the mixers it feeds, parents[parent_first[i] ..
  u32* parents = nullptr;       //   parent_first[i + 1])
};
constexpr u32 kGraphs = 9;
Graph g_graphs[kGraphs];  // System, GW3_FX, UIMusic, Song_1..6
u32 g_random_seed = 0;

float get_value(Graph& g, i32 idx, u32 out);
void fire(Graph& g, i32 idx);
bool process_audio(Graph& g, Comp& comp);

inline float read_slot(Graph& g, const u8* p) {
  const i32 src = bes(p);
  return src == -1 ? bef(p + 8) : get_value(g, src, be32(p + 4));
}
inline float read_in(Graph& g, const u8* p) { return get_value(g, bes(p), be32(p + 4)); }

u32 next_random() {
  g_random_seed = (g_random_seed + 0x18653) * 0x11BC9;
  return g_random_seed;
}

u32 body_size(const u8* b, u32 t) {
  static const struct {
    u32 t, size;
  } kFixed[] = {{0, 8},     {1, 0x10},   {2, 8},     {12, 4},    {13, 8},    {18, 8},    {24, 0x18},
                {26, 0x2C}, {29, 0x1C},  {38, 0xC},  {41, 0xC},  {46, 0x3C}, {48, 0x44}, {119, 0x38},
                {127, 0x40}, {130, 0x40}, {131, 0x40}, {133, 0x58}, {134, 0x58}, {146, 0x58}, {171, 0x10},
                {178, 0x7C}, {184, 4},    {185, 8}};
  for (auto& f : kFixed) {
    if (f.t == t) return f.size;
  }
  static const struct {
    u32 t, h;
  } kMixer[] = {{61, 0x18}, {62, 0x24}, {64, 0x18}, {65, 0x30}, {66, 0x60}, {69, 0x18},
                {181, 0x48}, {182, 0x24}, {183, 0x18}, {186, 0x78}, {187, 0xA8}};
  for (auto& m : kMixer) {
    if (m.t == t) return m.h + 4 * be32(b + m.h - 8) + 16 * be32(b + m.h - 4);
  }
  if (t == 31 || t == 32 || t == 36 || t == 37) return 8 + 12 * be32(b + 4);
  if (t == 19) return 0x1C + 16 * be32(b + 0x18);
  if (t == 49) return 0x1C + 12 * be32(b + 0x18);
  if (t == 170) return 0x14 + 48 * be32(b + 4);
  if (t == 169) return 0xC + 4 * be32(b + 8);
  if (t == 52 || t == 53) return 0x48 + 4 * be32(b + 0x44);
  if (t == 56 || t == 57) return 0x50 + 4 * be32(b + 0x4C);
  return 0;
}

// Channel counts as mixed here. The game mixes surround (L R C Ls Rs, and
// LFE); the Xbox's output is stereo, so every surround signal is carried
// already folded to stereo (L + 0.707 C + 0.707 Ls, R + 0.707 C + 0.707 Rs;
// LFE left out, as a standard stereo downmix does). The fold is linear, so
// folding each mixer's gains gives what folding the final mix would, at a
// fraction of the work (a surround panner mixes 2 channels instead of 5).
bool mixer_shape(u32 t, u32* h, u32* in_ch, u32* out_ch) {
  static const struct {
    u32 t, h, i, o;
  } k[] = {{61, 0x18, 1, 1}, {62, 0x24, 1, 2}, {64, 0x18, 2, 1}, {65, 0x30, 2, 2}, {66, 0x60, 2, 2},
           {69, 0x18, 2, 2}, {181, 0x48, 1, 2}, {182, 0x24, 2, 2}, {183, 0x18, 2, 2}, {186, 0x78, 2, 4},
           {187, 0xA8, 4, 2}};
  for (auto& m : k) {
    if (m.t == t) {
      *h = m.h;
      *in_ch = m.i;
      *out_ch = m.o;
      return true;
    }
  }
  return false;
}

// --- sound players --------------------------------------------------------------------
bool is_sound(u32 t) { return t == 52 || t == 53 || t == 56 || t == 57; }
bool is_looping(u32 t) { return t == 56 || t == 57; }

void sound_init(Graph& g, Comp& c) {
  const bool loop = is_looping(c.type);
  c.channels = (c.type == 53 || c.type == 57) ? 2 : 1;
  c.kind = kAudioSound;
  c.voice_count = loop ? 1 : (be32(c.b + 0xC) ? be32(c.b + 0xC) : 1);
  if (c.voice_count > 8) c.voice_count = 8;
  c.voices = arena_new<Voice>(c.voice_count);
  Bank* bank = nullptr;
  const Wave* w = find_wave(g.strings + be32(c.b + 4), g.strings + be32(c.b + 8), &bank);
  if (!w) gw_log("sound: %s: wave %s/%s not found\n", g.name, g.strings + be32(c.b + 4), g.strings + be32(c.b + 8));
  // A block must fit a stream ring's slot and the wave's blocks must hold
  // its samples; a wave that does not is refused (left silent), never
  // decoded past the end of its data.
  if (w && (w->channels < 1 || w->channels > 2 || w->block <= 4 * w->channels || w->block > kMaxBlock ||
            !w->samples || w->offset + w->size > GetFileSize(bank->file, nullptr) ||
            (w->xa ? (u64)(w->size / (36 * w->channels)) * 64
                   : (u64)((w->size + w->block - 1) / w->block) * ((w->block - 4 * w->channels) * 2 / w->channels + 1)) <
                w->samples)) {
    gw_log("sound: %s: wave %s has blocks this engine cannot hold\n", g.name, w->name);
    w = nullptr;
  }
  for (u32 v = 0; v < c.voice_count; ++v) {
    c.voices[v].r.bank = bank;
    c.voices[v].r.wave = w;
    c.voices[v].r.loop = loop;
  }
  const u32 octave = loop ? 0xC : 0x10, pitch = loop ? 0x18 : 0x1C;
  c.resample = !(w && w->rate == 48000 && bes(c.b + pitch) == -1 && bef(c.b + pitch + 8) == 1.0f &&
                 bes(c.b + octave) == -1 && bef(c.b + octave + 8) == 0.0f);
}

void sound_update(Graph& g, Comp& c) {
  const bool loop = is_looping(c.type);
  const u32 o_oct = loop ? 0xC : 0x10, o_pitch = loop ? 0x18 : 0x1C, o_db = loop ? 0x24 : 0x28,
            o_vol = loop ? 0x30 : 0x34;
  float pitch = read_slot(g, c.b + o_pitch);
  bool changed = false;
  if (pitch != c.cached[1]) {
    c.cached[1] = pitch;
    changed = true;
  }
  const float oct = read_slot(g, c.b + o_oct);
  if (oct != c.cached[0]) {
    c.cached[0] = oct;
    if (oct != 0) pitch *= (float)pow(2.0, oct);
    changed = true;
  }
  if (changed) {
    for (u32 v = 0; v < c.voice_count; ++v) c.voices[v].pitch = pitch;
  }
  float vol = read_slot(g, c.b + o_vol);
  if (c.type == 56 && vol < 0) vol = 0;
  bool vchanged = false;
  if (vol != c.cached[3]) {
    c.cached[3] = vol;
    vchanged = true;
  }
  const float db = read_slot(g, c.b + o_db);
  if (db != c.cached[2]) {
    c.cached[2] = db;
    if (db != 0) vol *= expf(db * 2.30259f * 0.05f);
    vchanged = true;
  }
  if (vchanged) {
    for (u32 v = 0; v < c.voice_count; ++v) c.voices[v].gain_target = vol < 16 ? vol : 16;
  }
  // progress output
  if (loop) {
    const Voice& v = c.voices[0];
    c.s[0] = v.r.wave && v.r.wave->samples ? (float)v.r.pos / (float)v.r.wave->samples : 0;
  } else if (c.last_voice >= 0) {
    const Voice& v = c.voices[c.last_voice];
    const float p = v.r.wave && v.r.wave->samples ? (float)v.r.pos / (float)v.r.wave->samples : 0;
    c.s[0] = p >= 1.0f ? 0 : p;
  }
}

void voice_start(Voice& v, u32 at) {
  if (!v.r.wave) return;
  v.r.start(at);
  v.silent_passes = 0;
  ring_take(v.r, &v.silent_passes);
  v.gain_prev = 0;
  v.gain_last = 0;
  // The resampler's carried sample and its filter start from silence: left
  // from the voice's last sound (a shot cut off by the next one), the new
  // sound began with a jump from there, a tick behind every effect.
  for (u32 cc = 0; cc < 2; ++cc) v.carry[cc] = v.fx1[cc] = v.fy1[cc] = 0;
  v.playing = true;
  v.target_step = v.prev_step = 1;
}

void hw_voice_stop(Voice& v);
void voice_stop(Voice& v) {
  v.playing = false;
  ring_give_back(v.r);
  hw_voice_stop(v);
}

// A voice reached the end of its wave: its finish action, and the count of
// playing voices (output 1) down.
void playing_changed(Graph& g, Comp& c);
void voice_end(Graph& g, Comp& c, Voice& v) {
  voice_stop(v);
  playing_changed(g, c);
  fire(g, bes(c.b + (is_looping(c.type) ? 0x48 : 0x40)));
  if (!is_looping(c.type)) c.s[1] = c.s[1] > 0 ? c.s[1] - 1 : 0;
  else c.s[1] = 0;
}

inline bool playing(const Comp& c) {
  for (u32 v = 0; v < c.voice_count; ++v)
    if (c.voices[v].playing) return true;
  return false;
}

// A sound's playing flag (and its graph's count) after its voices changed.
void playing_changed(Graph& g, Comp& c) {
  const u8 now = playing(c) ? 1 : 0;
  if (now == g.playing[c.index]) return;
  g.playing[c.index] = now;
  if (now) ++g.playing_count;
  else --g.playing_count;
}

void hw_voice_start(Graph& g, Comp& c, Voice& v);
void sound_action(Graph& g, Comp& c, u32 action) {
  sound_update(g, c);
  if (action == 0) {
    if (is_looping(c.type)) {
      Voice& v = c.voices[0];
      const float start = read_slot(g, c.b + 0x3C);
      c.s[1] = 1;
      voice_start(v, v.r.wave ? (u32)((float)v.r.wave->samples * start) : 0);
      hw_voice_start(g, c, v);
    } else {
      i32 v = c.last_voice + 1;
      if (v >= (i32)c.voice_count) v = 0;
      c.last_voice = v;
      if (!c.voices[v].playing) c.s[1] += 1;
      voice_start(c.voices[v], 0);
      hw_voice_start(g, c, c.voices[v]);
    }
  } else if (action == 1) {
    for (u32 v = 0; v < c.voice_count; ++v) voice_stop(c.voices[v]);
    c.s[1] = 0;
  }
  playing_changed(g, c);
}


// One voice's 256 samples into out (channels of c), gain applied; false if silent.
// *first: nothing written to out yet this pass (the first voice to sound
// writes it instead of adding, so out is never cleared for nothing).
bool voice_render(Graph& g, Comp& c, Voice& v, float (*out)[kPass], bool needed, bool* first) {
  if (!v.playing || !v.r.wave) return false;
  const u32 ch = c.channels;
  // Step for this pass (pitch and rate), clamped as the original.
  float step = (float)v.r.wave->rate * 2.08333e-5f * v.pitch;
  step = step - 1.0f / 256 >= 0 ? step : 1.0f / 256;
  step = 4 - step >= 0 ? step : 4;
  v.target_step = step;
  const float frac = (float)(g_pass % kDivisor) / (float)kDivisor;
  const float s = v.prev_step + (v.target_step - v.prev_step) * frac;
  if (g_pass % 6 == 5) v.prev_step = v.target_step;
  const u32 n = c.resample ? (u32)(256.0f * s + 0.5f) : kPass;
  // Gain ramp (one-shots: target over the period; looping: towards it).
  const float k = frac;
  float start, end;
  if (!is_looping(c.type)) {
    end = v.gain_target;
    start = g_pass % kDivisor == 0 ? v.gain_prev : v.gain_target;
  } else {
    start = v.gain_last;
    end = v.gain_prev + (v.gain_target - v.gain_prev) * (k + 1.0f / kDivisor);
  }
  v.gain_last = end;
  if (g_pass % kDivisor == kDivisor - 1) v.gain_prev = v.gain_target;
  // Inaudible: skip the samples, keep the place.
  if (!needed || (start < kSilentGain && end < kSilentGain && is_looping(c.type))) {
    ++v.silent_passes;
    if (!v.r.skip(n)) voice_end(g, c, v);
    return false;
  }
  // N input samples (with the carried one), filtered when resampling.
  static float in[2][4 * kPass + 2];
  bool ended = false;
  for (u32 cc = 0; cc < ch; ++cc) in[cc][0] = v.carry[cc];
  v.silent_passes = 0;
  if (v.r.streamed() && !v.r.ring) ring_take(v.r, &v.silent_passes);
  float* const dst[2] = {in[0], in[1]};
  PROF_BEGIN(decode);
  if (!v.r.streamed()) {
    pcm_cache(*v.r.wave);
    v.r.wave->pcm_used = g_pass;
  }
  const u32 got = v.r.render(dst, ch, 1, n, &ended);
  PROF_END(decode);
  ++gw_sound_voices;
  // A stream not there yet: silence, but the wave keeps its place (in time
  // with the music's other layers).
  if (got < n && !ended) {
    ++gw_sound_late;
    ++gw_sound_late_why[!v.r.ring ? 0 : v.r.ring_read == v.r.ring_written ? 1 : 2];
    gw_sound_late_wave = v.r.wave->name;
  }
  if (got < n && !ended && !v.r.skip(n - got)) ended = true;
  for (u32 i = got; i < n; ++i)
    for (u32 cc = 0; cc < ch; ++cc) in[cc][i + 1] = 0;
  if (c.resample) {
    if (s != v.f_step) {  // (the coefficients depend on the step alone)
      const float cut = s > 0.5f ? 0.5f / s : 1.0f;
      const float t = atanf(kPi * cut);
      v.fa = -(t - 1) / (t + 1);
      v.fb = (1 - v.fa) * 0.5f;
      v.f_step = s;
    }
    for (u32 cc = 0; cc < ch; ++cc) {
      float x1 = v.fx1[cc], y1 = v.fy1[cc];
      for (u32 i = 1; i <= n; ++i) {
        const float x = in[cc][i];
        y1 = v.fb * (x + x1) + v.fa * y1;
        x1 = x;
        in[cc][i] = y1;
      }
      v.fx1[cc] = x1;
      v.fy1[cc] = y1;
    }
  }
  const float gstep = (end - start) * (1.0f / 256);
  const bool write = *first;
  if (!write)
    for (u32 cc = 0; cc < ch; ++cc) prefetch(out[cc], kPass * 4);
  *first = false;
  for (u32 cc = 0; cc < ch; ++cc) {
    float* o = out[cc];
    const float* x = in[cc];
    if (n == kPass) {
      if (write) {
        for (u32 i = 0; i < kPass; ++i) o[i] = x[i + 1] * (start + gstep * (float)i);
      } else {
        for (u32 i = 0; i < kPass; ++i) o[i] += x[i + 1] * (start + gstep * (float)i);
      }
    } else {
      for (u32 i = 0; i < kPass; ++i) {
        const u32 p = i * n;
        const u32 at = p >> 8;
        const float f = (float)(p & 255) * (1.0f / 256);
        const float a = x[at], bb = x[at + 1];
        const float y = (a + (bb - a) * f) * (start + gstep * (float)i);
        o[i] = write ? y : o[i] + y;
      }
    }
    v.carry[cc] = x[n];
  }
  if (ended) voice_end(g, c, v);
  return true;
}

bool sound_process(Graph& g, Comp& c, bool needed) {
  if (!playing(c)) return false;
  bool any = false, first = true;
  PROF_BEGIN(sound);
  for (u32 v = 0; v < c.voice_count; ++v)
    if (c.voices[v].hw[0] < 0) any |= voice_render(g, c, c.voices[v], c.buf, needed, &first);
  PROF_END(sound);
  return any;
}

// --- mixers and their insert effects ----------------------------------------------------
void effect_process(Graph& g, Comp& fx, float (*buf)[kPass], u32 ch);

void mixer_init(Graph& g, Comp& c) {
  u32 h;
  mixer_shape(c.type, &h, &c.in_ch, &c.out_ch);
  c.kind = kAudioMixer;
  c.channels = c.out_ch;
  c.nA = be32(c.b + h - 8);
  c.nB = be32(c.b + h - 4);
  if (c.nB > kMaxInputs) {
    gw_log("sound: %s: %u inputs, only %u mixed\n", c.name, c.nB, kMaxInputs);
    c.nB = kMaxInputs;
  }
  const u32 n = c.nB * c.in_ch * c.out_ch;
  c.current = (float*)arena((n ? n : 1) * 4);
  c.target = (float*)arena((n ? n : 1) * 4);
  // per-type state
  if (c.type == 62) {
    c.s[0] = 0;  // last pan
    c.s[1] = c.s[2] = 0.707f;
  } else if (c.type == 64) {
    c.s[0] = 0;
    c.s[1] = c.s[2] = 0.707107f;
  } else if (c.type == 65) {
    c.s[0] = -0.5f;
    c.s[1] = 0.5f;
    c.s[2] = 1;
    c.s[3] = 0;
    c.s[4] = 0;
    c.s[5] = 1;
  }
}

inline float& gain_at(float* m, const Comp& c, u32 i, u32 ic, u32 o) {
  return m[(c.in_ch * i + ic) * c.out_ch + o];
}

// The surround panner (StereoToSurround / QuadToSurround / MonoToSurroundLFE).
const float kAngles[5] = {-0.666667f, -0.166667f, 0.0f, 0.166667f, 0.666667f};
float g_diffuse[5];
void init_diffuse() {
  const float* a = kAngles;
  float d[5];
  d[0] = (a[0] + 1) * 0.5f + (a[1] - a[0]) * 0.25f;
  for (int k = 1; k < 4; ++k) d[k] = (a[k] - a[k - 1]) * 0.25f + (a[k + 1] - a[k]) * 0.25f;
  d[4] = (a[4] - a[3]) * 0.25f + (0.5f - a[4] * 0.5f);
  for (int k = 0; k < 5; ++k) g_diffuse[k] = sqrtz(d[k]);
}
float wrap_angle(float x) {
  const int n = (int)x;
  if (x < -1) x -= 2.0f * (float)((n - 1) / 2);
  if (x > 1) x -= 2.0f * (float)((n + 1) / 2);
  return x;
}
void pan(float x, float* p) {
  const float* a = kAngles;
  if (x < a[0]) {
    const float t = (x + 2 - a[4]) / (a[0] + 2 - a[4]);
    p[0] = sqrtz(t);
    p[4] = sqrtz(1 - t);
  } else if (x > a[4]) {
    const float t = (x - a[4]) / (a[0] + 2 - a[4]);
    p[0] = sqrtz(t);
    p[4] = sqrtz(1 - t);
  } else {
    for (int j = 1; j < 5; ++j) {
      if (x < a[j]) {
        const float t = (x - a[j - 1]) / (a[j] - a[j - 1]);
        p[j] = sqrtz(t);
        p[j - 1] = sqrtz(1 - t);
        break;
      }
    }
  }
}
const int kSpeaker5[5] = {1, 3, 2, 0, 4};  // outputs L R C Ls Rs <- speakers
// Surround gains (L R C Ls Rs) folded to stereo (see mixer_shape): lr[0] the
// gain to L, lr[1] to R (adjacent in the gain matrix).
inline void fold(const float* v, float* lr) {
  const float h = 0.70710678f;
  lr[0] = v[0] + h * v[2] + h * v[3];
  lr[1] = v[1] + h * v[2] + h * v[4];
}

void mixer_update(Graph& g, Comp& c) {
  u32 h, ic, oc;
  mixer_shape(c.type, &h, &ic, &oc);
  const u8* entries = c.b + h + 4 * c.nA;
  auto gain = [&](u32 i) { return read_slot(g, entries + i * 16 + 4); };
  switch (c.type) {
    case 61: {
      const float m = read_slot(g, c.b + 4);
      for (u32 i = 0; i < c.nB; ++i) gain_at(c.target, c, i, 0, 0) = gain(i) * m;
      break;
    }
    case 62: {
      const float m = read_slot(g, c.b + 4), p = read_slot(g, c.b + 0x10);
      if (p != c.s[0]) {
        c.s[0] = p;
        c.s[1] = sqrtz((1 - p) * 0.5f);
        c.s[2] = sqrtz((p + 1) * 0.5f);
      }
      for (u32 i = 0; i < c.nB; ++i) {
        const float f = gain(i) * m;
        gain_at(c.target, c, i, 0, 0) = c.s[1] * f;
        gain_at(c.target, c, i, 0, 1) = c.s[2] * f;
      }
      break;
    }
    case 64: {
      const float bal = clampf(read_slot(g, c.b + 4), -1, 1);
      if (bal != c.s[0]) {
        c.s[0] = bal;
        c.s[1] = sqrtz(0.5f - bal * 0.5f);
        c.s[2] = sqrtz(bal * 0.5f + 0.5f);
      }
      for (u32 i = 0; i < c.nB; ++i) {
        const float gg = gain(i);
        gain_at(c.target, c, i, 0, 0) = c.s[1] * gg;
        gain_at(c.target, c, i, 1, 0) = c.s[2] * gg;
      }
      break;
    }
    case 65: {
      const float m = read_slot(g, c.b + 4), pl = read_slot(g, c.b + 0x10), pr = read_slot(g, c.b + 0x1C);
      if (pl != c.s[0]) {
        c.s[0] = pl;
        c.s[2] = sqrtz((1 - pl) * 0.5f);
        c.s[3] = sqrtz((pl + 1) * 0.5f);
      }
      if (pr != c.s[1]) {
        c.s[1] = pr;
        c.s[4] = sqrtz((1 - pr) * 0.5f);
        c.s[5] = sqrtz((pr + 1) * 0.5f);
      }
      for (u32 i = 0; i < c.nB; ++i) {
        const float gg = gain(i);
        gain_at(c.target, c, i, 0, 0) = (c.s[2] * gg) * m;
        gain_at(c.target, c, i, 0, 1) = (c.s[3] * gg) * m;
        gain_at(c.target, c, i, 1, 0) = (gg * m) * c.s[4];
        gain_at(c.target, c, i, 1, 1) = (c.s[5] * gg) * m;
      }
      break;
    }
    case 66: {  // StereoToSurround (X 0 and Dist FLT_MAX in the data: q = 1, wA 0, wB 1)
      const float m = read_slot(g, c.b + 4);
      float pl[5] = {}, pr[5] = {}, v[5];
      pan(wrap_angle(read_slot(g, c.b + 0x10)), pl);
      pan(wrap_angle(read_slot(g, c.b + 0x1C)), pr);
      for (u32 i = 0; i < c.nB; ++i) {
        const float f = gain(i) * m;
        for (u32 o = 0; o < 5; ++o) v[o] = pl[kSpeaker5[o]] * f;
        fold(v, &gain_at(c.target, c, i, 0, 0));
        for (u32 o = 0; o < 5; ++o) v[o] = pr[kSpeaker5[o]] * f;
        fold(v, &gain_at(c.target, c, i, 1, 0));
      }
      break;
    }
    case 187: {  // QuadToSurround
      const float m = read_slot(g, c.b + 4);
      float p[4][5] = {};
      float wa[4], wb[4], v[5];
      for (u32 q = 0; q < 4; ++q) {
        const u8* e = c.b + 0x10 + q * 0x24;
        pan(wrap_angle(read_slot(g, e)), p[q]);
        const float qq = clampf(read_slot(g, e + 0x18) / 2.0f, 0, 1);
        wa[q] = sqrtz(1 - qq);
        wb[q] = sqrtz(qq);
      }
      for (u32 i = 0; i < c.nB; ++i) {
        const float f = gain(i) * m;
        for (u32 q = 0; q < 4; ++q) {
          for (u32 o = 0; o < 5; ++o) v[o] = (g_diffuse[kSpeaker5[o]] * wa[q] + p[q][kSpeaker5[o]] * wb[q]) * f;
          fold(v, &gain_at(c.target, c, i, q, 0));
        }
      }
      break;
    }
    case 181: {  // MonoToSurroundLFE (its LFE send left out)
      const float m = read_slot(g, c.b + 4);
      float p[5] = {}, v[5];
      pan(wrap_angle(read_slot(g, c.b + 0x10)), p);
      const float q = clampf(read_slot(g, c.b + 0x28) / 2.0f, 0, 1);
      const float wa = sqrtz(1 - q), wb = sqrtz(q);
      for (u32 i = 0; i < c.nB; ++i) {
        const float f = gain(i) * m;
        for (u32 o = 0; o < 5; ++o) v[o] = (g_diffuse[kSpeaker5[o]] * wa + p[kSpeaker5[o]] * wb) * f;
        fold(v, &gain_at(c.target, c, i, 0, 0));
      }
      break;
    }
    case 69:   // SurroundToSurround: a gain on every channel
    case 182:  // SurroundToSurroundLFE (the LFE send left out)
    case 183: {  // SurroundLFEToSurroundLFE
      const float m = read_slot(g, c.b + 4);
      for (u32 i = 0; i < c.nB; ++i) {
        const float f = gain(i) * m;
        gain_at(c.target, c, i, 0, 0) = f;
        gain_at(c.target, c, i, 1, 1) = f;
      }
      break;
    }
    case 186: {
      float mm[8];
      for (u32 k = 0; k < 8; ++k) mm[k] = read_slot(g, c.b + 0x10 + k * 12);
      for (u32 i = 0; i < c.nB; ++i) {
        const float gg = gain(i);
        for (u32 o = 0; o < 4; ++o) {
          gain_at(c.target, c, i, 0, o) = gg * mm[o];
          gain_at(c.target, c, i, 1, o) = gg * mm[4 + o];
        }
      }
      break;
    }
  }
}

// --- mixing ---------------------------------------------------------------------------
// The game's sounds reach an output through chains of mixers, most of which
// only scale and pan. The chains are combined once per control period (two
// passes): every sound, input and mixer with insert effects is mixed once,
// straight into the nearest mixer with effects (or the root an output port
// pulls), at the product of the gains along the way, summed over the ways.
// The product is taken at the period's start, middle and end, and ramps
// linearly between them across each pass (the original ramps each mixer's
// own gains, which differs from this only in a curvature within 5 ms). A
// mixer's gains are worked out once per control pass, and only while
// something under it plays. A source that starts within a period joins the
// mix at the next one (at most 5 ms later).
constexpr u32 kMaxContribs = 1024;
constexpr u32 kMaxVisited = 256;
struct Contrib {
  Comp* src;
  float a[3][16];  // [period start, middle, end][source channel * 4 + destination channel]
};
Contrib g_contribs[kMaxContribs];
u32 g_contrib_top;            // reset at each control pass
Comp* g_visited[kMaxVisited];  // mixers whose gains were used this period
u32 g_visited_count;
bool g_contrib_full_logged;

inline bool has_inserts(Graph& g, const Comp& c) {
  u32 h, ic, oc;
  mixer_shape(c.type, &h, &ic, &oc);
  for (u32 a = 0; a < c.nA; ++a)
    if (bes(c.b + h + a * 4) >= 0) return true;
  (void)g;
  return false;
}

// Whether anything under c can sound this pass (marked upwards from what
// plays at the start of the pass, mark_live).
inline bool live(Graph& g, const Comp& c) { return g.live_pass[c.index] == g_pass; }
void mark_live(Graph& g, u32 i) {
  if (g.live_pass[i] == g_pass) return;
  g.live_pass[i] = g_pass;
  for (u32 k = g.parent_first[i]; k < g.parent_first[i + 1]; ++k) mark_live(g, g.parents[k]);
}

void gains_ready(Graph& g, Comp& c) {
  if (c.updated != g_epoch) {
    c.updated = g_epoch;
    mixer_update(g, c);
  }
  if (c.gain_stamp != g_epoch) {
    c.gain_stamp = g_epoch;
    if (g_visited_count < kMaxVisited) g_visited[g_visited_count++] = &c;
  }
}

// Adds what the sources of mixer m contribute, through matrices A (m's
// outputs -> the destination's dch channels, [o * 4 + d], at the period's
// start, middle and end), to the list.
void collect(Graph& g, Comp& m, const float (*A)[16], u32 dch) {
  u32 h, ic, oc;
  mixer_shape(m.type, &h, &ic, &oc);
  const u8* entries = m.b + h + 4 * m.nA;
  for (u32 i = 0; i < m.nB; ++i) {
    const i32 src = bes(entries + i * 16);
    if (src < 0) continue;
    Comp& s = g.c[src];
    if (!live(g, s)) continue;
    // (A way at zero gain is still taken: what is under it must keep its
    // place, e.g. the songs' silent sync tracks, whose progress the music
    // follows.)
    // The products into the contribution itself when s is mixed here (no
    // copy), on the stack when the chain goes on through s.
    const bool through = s.kind == kAudioMixer && !s.inserts;
    float local[3][16];
    float (*C)[16];
    if (through) {
      C = local;
    } else if (g_contrib_top < kMaxContribs) {
      C = g_contribs[g_contrib_top].a;
    } else {
      C = nullptr;
    }
    if (C) {
      memset(C, 0, sizeof(local));
      for (u32 x = 0; x < m.in_ch; ++x) {
        const u32 sc = x < s.channels ? x : 0;
        for (u32 o = 0; o < m.out_ch; ++o) {
          const float t = gain_at(m.target, m, i, x, o), cur = gain_at(m.current, m, i, x, o);
          if (t == 0 && cur == 0) continue;
          const float gs[3] = {cur, (t + cur) * 0.5f, t};
          for (u32 p = 0; p < 3; ++p)
            for (u32 d = 0; d < dch; ++d) C[p][sc * 4 + d] += gs[p] * A[p][o * 4 + d];
        }
      }
    }
    if (through) {
      gains_ready(g, s);
      collect(g, s, C, dch);
    } else if (C) {
      g_contribs[g_contrib_top++].src = &s;
    } else if (!g_contrib_full_logged) {
      g_contrib_full_logged = true;
      gw_log("sound: more than %u mixes in one period; the rest left out\n", kMaxContribs);
    }
  }
}

bool compute(Graph& g, Comp& d);
#ifdef GW_SOUND_APU
void hw_route_add(Graph& g, Comp& d, Comp& s, const float* A);
bool hw_bins_add(Graph& g, Comp& d, bool* fresh);
#endif

// A contribution's source with its audio ready this pass; false if silent.
bool source_ready(Graph& g, Comp& s, float gain) {
  switch (s.kind) {
    case kAudioSound:
      // Skipped undecoded while every way it is mixed is near silent (as of
      // the last pass, d[0], and so far this pass, d[1]).
      if (s.audio_stamp != g_pass) {
        s.audio_stamp = g_pass;
        s.d[0] = s.d[1];
        s.d[1] = gain;
        s.active = sound_process(g, s, fmaxf(s.d[0], gain) >= kSilentGain);
      } else {
        s.d[1] = fmaxf(s.d[1], gain);
      }
      return s.active;
    case kAudioInput:
      return s.flag[3] != 0;  // it had sound last pass (else its buffer is old)
    case kAudioMixer:
      return compute(g, s);
    default:
      return false;
  }
}

// Mixer d's output into its buffer (its inserts run on it); false if silent.
bool compute(Graph& g, Comp& d) {
  if (d.audio_stamp == g_pass) return d.active;
  d.audio_stamp = g_pass;
  d.active = false;
  if (!live(g, d)) return false;
  if (d.collected != g_epoch) {  // the period's mixes into d
    d.collected = g_epoch;
    gains_ready(g, d);
    float I[3][16] = {};
    for (u32 p = 0; p < 3; ++p)
      for (u32 o = 0; o < d.out_ch; ++o) I[p][o * 4 + o] = 1;
    d.contrib_first = g_contrib_top;
    PROF_BEGIN(collect);
    collect(g, d, I, d.out_ch);
    PROF_END(collect);
    d.contrib_end = g_contrib_top;
  }
  const u32 half = g_pass % kDivisor;  // this pass: from point half to half + 1
  // Each output channel is written by its first mix and added to after (and
  // cleared at the end only if nothing reached it).
  bool fresh[4] = {true, true, true, true};
  bool any = false;
  for (u32 o = 0; o < d.out_ch; ++o) prefetch(d.buf[o], kPass * 4);
  for (u32 k = d.contrib_first; k < d.contrib_end; ++k) {
    const Contrib& t = g_contribs[k];
    const float* A0 = t.a[half];
    const float* A1 = t.a[half + 1];
    Comp& s = *t.src;
    float peak = 0;
    for (u32 x = 0; x < 16; ++x) peak = fmaxf(peak, fmaxf(fabsf(A0[x]), fabsf(A1[x])));
#ifdef GW_SOUND_APU
    if (d.apu_out && s.apu_ok) hw_route_add(g, d, s, A1);
#endif
    if (!source_ready(g, s, peak)) continue;
    any = true;
    PROF_BEGIN(mix);
    for (u32 c = 0; c < s.channels && c < 4; ++c)
      prefetch(s.kind == kAudioInput ? s.in_now[c] : s.buf[c], kPass * 4);
    for (u32 c = 0; c < s.channels && c < 4; ++c) {
      const float* in = s.kind == kAudioInput ? s.in_now[c] : s.buf[c];
      for (u32 o = 0; o < d.out_ch; ++o) {
        const float a0 = A0[c * 4 + o], a1 = A1[c * 4 + o];
        float* out = d.buf[o];
        if (a0 == 0 && a1 == 0) continue;
        if (fresh[o]) {
          fresh[o] = false;
          if (a0 == a1) {
            for (u32 n = 0; n < kPass; ++n) out[n] = in[n] * a0;
          } else {
            const float st = (a1 - a0) * (1.0f / 256);
            for (u32 n = 0; n < kPass; ++n) out[n] = in[n] * ((float)n * st + a0);
          }
        } else if (a0 == a1) {
          if (a0 == 1) {
            for (u32 n = 0; n < kPass; ++n) out[n] += in[n];
          } else {
            for (u32 n = 0; n < kPass; ++n) out[n] += in[n] * a0;
          }
        } else {
          const float st = (a1 - a0) * (1.0f / 256);
          for (u32 n = 0; n < kPass; ++n) out[n] += in[n] * ((float)n * st + a0);
        }
      }
    }
    PROF_END(mix);
  }
#ifdef GW_SOUND_APU
  if (d.apu_out && d.apu_refs) any |= hw_bins_add(g, d, fresh);
#endif
  if (!any) return false;
  for (u32 o = 0; o < d.out_ch; ++o)
    if (fresh[o]) memset(d.buf[o], 0, sizeof(d.buf[o]));
  if (d.inserts) {
    u32 h, ic, oc;
    mixer_shape(d.type, &h, &ic, &oc);
    for (u32 a = 0; a < d.nA; ++a) {
      const i32 fx = bes(d.b + h + a * 4);
      if (fx >= 0) {
        PROF_BEGIN(effects);
        effect_process(g, g.c[fx], d.buf, d.out_ch);
        PROF_END(effects);
      }
    }
  }
  d.active = true;
  return true;
}

// End of a pass: at the end of a control period each used mixer's gains
// have reached their targets.
void gains_pass_done() {
  if (g_pass % kDivisor == kDivisor - 1) {
    for (u32 i = 0; i < g_visited_count; ++i) {
      Comp& c = *g_visited[i];
      memcpy(c.current, c.target, c.nB * c.in_ch * c.out_ch * sizeof(float));
    }
    g_visited_count = 0;
  }
}

// Effects (inserts on mixers), parameters set in their control update.
// The libm calls (pow, exp, atan, sin...) are slow on the Xbox, so each
// effect keeps the last inputs and results of its own (d[24..31], which no
// effect's state uses): recomputed only when an input changes.
float freq_group(Graph& g, Comp& c) {
  const u8* b = c.b;
  const float oct = read_slot(g, b + 0x10);
  if (!(oct == c.d[24]) || !c.d[25]) {
    c.d[24] = oct;
    c.d[25] = (float)pow(2.0, oct);
  }
  float f = c.d[25] * (read_slot(g, b + 0x1C) * read_slot(g, b + 4));
  f = f - 20 >= 0 ? f : 20;
  f = 24000 - f >= 0 ? f : 24000;
  return f;
}
float db_lin(Comp& c, float e) {
  if (!(e == c.d[26]) || !c.d[27]) {
    c.d[26] = e;
    c.d[27] = (float)exp((double)((e * 2.3025851f) * 0.05f));
  }
  return c.d[27];
}

void effect_init(Graph& g, Comp& c) {
  (void)g;
  switch (c.type) {
    case 127:  // LowPass: target, current, gain, last
      c.d[0] = c.d[1] = 24000;
      c.d[2] = 1;
      c.d[3] = -1;
      break;
    case 130:
    case 131:
      c.d[0] = 24000;
      c.d[1] = 1;
      c.d[2] = -1;
      break;
    case 133:
    case 134:
      c.d[0] = c.d[1] = 24000;
      c.d[2] = 0;
      c.d[3] = 1;
      c.d[4] = c.d[5] = c.d[6] = -1;
      break;
    case 146:
      c.d[0] = 440;
      c.d[1] = 0;
      c.d[2] = 1;
      c.d[3] = 3.4e38f;
      c.d[4] = c.d[5] = c.d[6] = -1;
      break;
    case 178:
      c.d[0] = 440;
      c.d[1] = 1;
      c.d[2] = 1;
      c.d[4] = c.d[5] = c.d[6] = -1;
      break;
    case 119: {
      const float mx = bef(c.b + 4);
      c.delay_blocks = (u32)((((mx > 0 ? mx : 0) * 0.00390625f) * 48000.0f));
      if (c.delay_blocks < 1) c.delay_blocks = 1;
      c.delay = (float*)arena((c.delay_blocks + 1) * 2 * kPass * 4);
      c.d[0] = 0;  // fb
      c.d[1] = 1;  // wet
      c.d[2] = 1;  // dry
      c.d[3] = -1;  // delay blocks (unset)
      c.d[4] = 0;   // last delay
      c.d[5] = 0;   // write index
      break;
    }
  }
}

void effect_update(Graph& g, Comp& c) {
  switch (c.type) {
    case 127:
    case 130:
    case 131:
      c.d[0] = freq_group(g, c);
      c.d[c.type == 127 ? 2 : 1] = fmaxf(db_lin(c, read_slot(g, c.b + 0x28)) * read_slot(g, c.b + 0x34), 0);
      break;
    case 133:
    case 134:
      c.d[0] = freq_group(g, c);
      c.d[2] = clampf(read_slot(g, c.b + 0x28), 0, 1);
      c.d[3] = fmaxf(db_lin(c, read_slot(g, c.b + 0x34)) * read_slot(g, c.b + 0x40), 0);
      break;
    case 146:
      c.d[0] = freq_group(g, c);
      c.d[1] = clampf(read_slot(g, c.b + 0x28), 0, 1);
      c.d[2] = fmaxf(db_lin(c, read_slot(g, c.b + 0x34)) * read_slot(g, c.b + 0x40), 0.0001f);
      c.d[3] = fminf(read_slot(g, c.b + 0x4C), 1);
      break;
    case 178:
      c.d[2] = fmaxf(db_lin(c, read_slot(g, c.b + 4)) * read_slot(g, c.b + 0x10), 0.0001f);
      c.d[0] = clampf(read_slot(g, c.b + 0x1C), 20, 24000);
      c.d[1] = clampf(read_slot(g, c.b + 0x28), 0.0001f, 1);
      break;
    case 119: {
      const float t = read_slot(g, c.b + 8);
      u32 dly = (u32)(187.5f * t);
      if (dly > c.delay_blocks - 1) dly = c.delay_blocks - 1;
      c.d[3] = (float)dly;
      c.d[0] = clampf(read_slot(g, c.b + 0x14), -1, 1);
      c.d[1] = fmaxf(read_slot(g, c.b + 0x20), 0);
      c.d[2] = fmaxf(read_slot(g, c.b + 0x2C), 0);
      break;
    }
  }
}

void effect_process(Graph& g, Comp& c, float (*buf)[kPass], u32 ch) {
  if (c.updated != g_epoch) {
    c.updated = g_epoch;
    effect_update(g, c);
  }
  float* d = c.d;
  for (u32 cc = 0; cc < ch && cc < 2; ++cc) prefetch(buf[cc], kPass * 4);
  switch (c.type) {
    case 127: {  // one-pole LP, slew-limited cutoff
      const float m = (d[1] * 256.0f) * 0.001953125f;
      d[1] += clampf(d[0] - d[1], -m, m);
      if (d[1] != d[3]) {
        const float t = atanf((d[1] * (1 / 48000.f)) * kPi);
        d[3] = d[1];
        d[9] = -((t - 1) / (t + 1));
        d[8] = (1 - d[9]) * 0.5f;
      }
      // (Coefficients and state in registers, two channels a loop: the
      // same operations in the same order as one channel at a time.)
      const float a = d[8], b = d[9], gain = d[2];
      u32 cc = 0;
      for (; cc + 1 < ch && cc + 1 < 6; cc += 2) {
        float* X0 = buf[cc];
        float* X1 = buf[cc + 1];
        float p0 = d[10 + cc], q0 = d[16 + cc], p1 = d[11 + cc], q1 = d[17 + cc];
        for (u32 n = 0; n < kPass; ++n) {
          const float x0 = X0[n], x1 = X1[n];
          q0 = (x0 + p0) * a + b * q0;
          q1 = (x1 + p1) * a + b * q1;
          p0 = x0;
          p1 = x1;
          X0[n] = gain * q0;
          X1[n] = gain * q1;
        }
        d[10 + cc] = p0;
        d[16 + cc] = q0;
        d[11 + cc] = p1;
        d[17 + cc] = q1;
      }
      for (; cc < ch && cc < 6; ++cc) {
        float* X = buf[cc];
        float x1 = d[10 + cc], y1 = d[16 + cc];
        for (u32 n = 0; n < kPass; ++n) {
          const float x = X[n];
          y1 = (x + x1) * a + b * y1;
          x1 = x;
          X[n] = gain * y1;
        }
        d[10 + cc] = x1;
        d[16 + cc] = y1;
      }
      break;
    }
    case 130:
    case 131: {  // one-pole HP, inverted polarity
      if (d[0] != d[2]) {
        const float t = atanf(((1 / 48000.f) * kPi) * d[0]);
        d[2] = d[0];
        d[4] = -((t - 1) / (t + 1));
        d[3] = (d[4] + 1) * 0.5f;
      }
      const float a = d[4], b = d[3], gain = d[1];
      if (ch >= 2) {
        float* X0 = buf[0];
        float* X1 = buf[1];
        float p0 = d[8], q0 = d[12], p1 = d[9], q1 = d[13];
        for (u32 n = 0; n < kPass; ++n) {
          const float x0 = X0[n], x1 = X1[n];
          q0 = a * q0 + b * (p0 - x0);
          q1 = a * q1 + b * (p1 - x1);
          p0 = x0;
          p1 = x1;
          X0[n] = q0 * gain;
          X1[n] = q1 * gain;
        }
        d[8] = p0;
        d[12] = q0;
        d[9] = p1;
        d[13] = q1;
      } else if (ch == 1) {
        float* X = buf[0];
        float x1 = d[8], y1 = d[12];
        for (u32 n = 0; n < kPass; ++n) {
          const float x = X[n];
          y1 = a * y1 + b * (x1 - x);
          x1 = x;
          X[n] = y1 * gain;
        }
        d[8] = x1;
        d[12] = y1;
      }
      break;
    }
    case 133:
    case 134: {  // RBJ low pass (w = pi f / 48000), gain applied twice
      if (!(d[2] == d[28]) || !d[29]) {
        d[28] = d[2];
        d[29] = 1.0f / (float)pow(2.0, d[2] * 3);
      }
      const float m = (d[29] * d[1]) * 256 * 0.001953125f;
      d[1] += clampf(d[0] - d[1], -m, m);
      if (d[1] != d[4] || d[2] != d[5] || d[3] != d[6]) {
        const double q = pow(2.0, d[2] * 8), w = (d[1] * (1 / 48000.f)) * kPi;
        const double s = sin(w), cs = cos(w), alpha = s / (q * 2.0), inv = 1.0 / (alpha + 1.0);
        d[4] = d[1];
        d[5] = d[2];
        d[6] = d[3];
        d[7] = (float)(cs * -2.0 * inv);
        d[8] = (float)((1.0 - alpha) * inv);
        d[10] = (float)((1.0 - cs) * inv) * d[3];
        d[9] = d[10] * 0.5f;
      }
      const float b0 = d[9], b1 = d[10], na1 = -d[7], na2 = -d[8], gain = d[3];
      auto step = [&](float x, float* st) {  // st: x1 x2 y1 y2 (registers in the loops below)
        const float fir = b0 * x + b1 * st[0] + b0 * st[1];
        float y = st[2] * na1 + fir;
        y = st[3] * na2 + y;
        st[1] = st[0];
        st[0] = x;
        st[3] = st[2];
        st[2] = y;
        return y * gain;
      };
      if (ch >= 2) {
        float* X0 = buf[0];
        float* X1 = buf[1];
        float s0[4] = {d[12], d[13], d[14], d[15]}, s1[4] = {d[16], d[17], d[18], d[19]};
        for (u32 n = 0; n < kPass; ++n) {
          const float y0 = step(X0[n], s0), y1 = step(X1[n], s1);
          X0[n] = y0;
          X1[n] = y1;
        }
        for (u32 k = 0; k < 4; ++k) {
          d[12 + k] = s0[k];
          d[16 + k] = s1[k];
        }
      } else if (ch == 1) {
        float* X = buf[0];
        float s0[4] = {d[12], d[13], d[14], d[15]};
        for (u32 n = 0; n < kPass; ++n) X[n] = step(X[n], s0);
        for (u32 k = 0; k < 4; ++k) d[12 + k] = s0[k];
      }
      break;
    }
    case 146: {  // RBJ peaking EQ + clip
      if (d[0] != d[4] || d[1] != d[5] || d[2] != d[6]) {
        const float q = (float)pow(2.0, d[1] * 8), w = ((1 / 48000.f) * 6.2831855f) * d[0];
        const float s = sinf(w), cs = cosf(w), alpha = s / (q * 2), aA = alpha / d[2], Aa = d[2] * alpha,
                    inv = 1 / (aA + 1);
        d[4] = d[0];
        d[5] = d[1];
        d[6] = d[2];
        d[8] = (1 - aA) * inv;
        d[7] = (cs * -2) * inv;
        d[9] = (Aa + 1) * inv;
        d[10] = (1 - Aa) * inv;
      }
      const float c7 = d[7], c9 = d[9], c10 = d[10], n7 = -d[7], n8 = -d[8], hi = d[3], lo = -d[3];
      auto step = [&](float x, float* st) {  // st: x1 x2 y1 y2 (registers in the loops below)
        float t = c7 * st[0];
        t = c9 * x + t;
        t = c10 * st[1] + t;
        t = n7 * st[2] + t;
        float y = n8 * st[3] + t;
        y = clampf(y, lo, hi);
        st[1] = st[0];
        st[0] = x;
        st[3] = st[2];
        st[2] = y;
        return y;
      };
      if (ch >= 2) {
        float* X0 = buf[0];
        float* X1 = buf[1];
        float s0[4] = {d[12], d[13], d[14], d[15]}, s1[4] = {d[16], d[17], d[18], d[19]};
        for (u32 n = 0; n < kPass; ++n) {
          const float y0 = step(X0[n], s0), y1 = step(X1[n], s1);
          X0[n] = y0;
          X1[n] = y1;
        }
        for (u32 k = 0; k < 4; ++k) {
          d[12 + k] = s0[k];
          d[16 + k] = s1[k];
        }
      } else if (ch == 1) {
        float* X = buf[0];
        float s0[4] = {d[12], d[13], d[14], d[15]};
        for (u32 n = 0; n < kPass; ++n) X[n] = step(X[n], s0);
        for (u32 k = 0; k < 4; ++k) d[12 + k] = s0[k];
      }
      break;
    }
    case 178: {  // low shelf (stage 0 of the distortion)
      if (d[0] != d[4] || d[1] != d[5] || d[2] != d[6]) {
        const float w = (float)((d[0] * (1 / 48000.)) * 6.2831855);
        const double s = sinf(w), cs = cosf(w), A = d[2], beta = sqrt(A) * s / d[1];
        const double am = A - 1, ap = A + 1, inv = 1 / (ap + am * cs + beta);
        d[4] = d[0];
        d[5] = d[1];
        d[6] = d[2];
        d[9] = (float)(A * (ap - am * cs + beta) * inv);
        d[10] = (float)(2 * A * (am - ap * cs) * inv);
        d[11] = (float)(A * (ap - am * cs - beta) * inv);
        d[7] = (float)(-2 * (am + ap * cs) * inv);
        d[8] = (float)((ap + am * cs - beta) * inv);
      }
      const float c9 = d[9], c10 = d[10], c11 = d[11], n7 = -d[7], n8 = -d[8];
      auto step = [&](float x, float* st) {  // st: x1 x2 y1 y2 (registers in the loops below)
        float t = c10 * st[0];
        t = c9 * x + t;
        t = c11 * st[1] + t;
        t = n7 * st[2] + t;
        const float y = n8 * st[3] + t;
        st[1] = st[0];
        st[0] = x;
        st[3] = st[2];
        st[2] = y;
        return y;
      };
      if (ch >= 2) {
        float* X0 = buf[0];
        float* X1 = buf[1];
        float s0[4] = {d[12], d[13], d[14], d[15]}, s1[4] = {d[16], d[17], d[18], d[19]};
        for (u32 n = 0; n < kPass; ++n) {
          const float y0 = step(X0[n], s0), y1 = step(X1[n], s1);
          X0[n] = y0;
          X1[n] = y1;
        }
        for (u32 k = 0; k < 4; ++k) {
          d[12 + k] = s0[k];
          d[16 + k] = s1[k];
        }
      } else if (ch == 1) {
        float* X = buf[0];
        float s0[4] = {d[12], d[13], d[14], d[15]};
        for (u32 n = 0; n < kPass; ++n) X[n] = step(X[n], s0);
        for (u32 k = 0; k < 4; ++k) d[12 + k] = s0[k];
      }
      break;
    }
    case 119: {  // block-quantised stereo feedback delay
      const u32 dly = (u32)d[3], N = c.delay_blocks;
      if (d[3] < 0) break;
      if (dly != (u32)d[4]) {
        memset(c.delay, 0, (size_t)(N + 1) * 2 * kPass * sizeof(float));
        d[4] = (float)dly;
      }
      const u32 w = (u32)d[5];
      u32 r = dly + w;
      if (r >= N) r -= N;
      for (u32 cc = 0; cc < ch && cc < 2; ++cc) {
        float* X = buf[cc];
        float* R = c.delay + ((size_t)cc * (N + 1) + r) * kPass;
        float* W = c.delay + ((size_t)cc * (N + 1) + w) * kPass;
        if (dly == 0) {
          if (d[2] != 1)
            for (u32 n = 0; n < kPass; ++n) X[n] *= d[2];
          continue;
        }
        const float fb = d[0], wet = d[1], dry = d[2];
        for (u32 n = 0; n < kPass; ++n) {
          const float x = X[n], wv = W[n];
          if (fb != 0) R[n] = fb * (x + wv);
          X[n] = x * dry + wet * wv;
        }
      }
      u32 nw = w + 1;
      if (nw >= N) nw = 0;
      d[5] = (float)nw;
      break;
    }
  }
}

// --- values ---------------------------------------------------------------------------
float get_value(Graph& g, i32 idx, u32 out) {
  if (idx < 0 || (u32)idx >= g.count) return 0;
  Comp& c = g.c[idx];
  const u8* b = c.b;
  switch (c.type) {
    case 0:   // ParameterIn
    case 18:  // Variable
      return c.s[0];
    case 1:  // ParameterOut
      return c.s[0];
    case 19: {  // PiecewiseLinearFunction
      if (c.stamp == g_pass) return c.value;
      c.stamp = g_pass;
      const u32 n = be32(b + 0x18);
      const u8* p = b + 0x1C;
      auto px = [&](u32 i) { return bef(p + i * 16); };
      auto py = [&](u32 i) {
        const i32 src = bes(p + i * 16 + 8);
        return src == -1 ? bef(p + i * 16 + 4) : get_value(g, src, be32(p + i * 16 + 12));
      };
      const float x = read_in(g, b + 4), cv = read_slot(g, b + 0xC);
      if (!n) return c.value = 0;
      if (x <= px(0)) {
        c.value = py(0);
      } else if (x >= px(n - 1)) {
        c.value = py(n - 1);
      } else {
        u32 i = 1;
        while (i < n && px(i) < x) ++i;
        if (x == px(i)) {
          c.value = py(i);
        } else {
          float t = (x - px(i - 1)) / (px(i) - px(i - 1)), a = 1 - t;
          if (cv != 1) {
            a = (float)pow(a, cv);
            t = (float)pow(t, cv);
          }
          c.value = py(i) * t + a * py(i - 1);
        }
      }
      return c.value;
    }
    case 24:  // control LowPass
      return c.s[3];
    case 26:  // Timer
      return out == 1 ? (c.flag[0] ? 1.0f : 0.0f) : c.s[0];
    case 29:   // Random
    case 170:  // Selector
      return c.s[0];
    case 31:
    case 32:
    case 36:
    case 37: {
      if (c.stamp == g_pass) return c.value;
      c.stamp = g_pass;
      const u32 n = be32(b + 4);
      float acc = n ? read_slot(g, b + 8) : 0;
      for (u32 k = 1; k < n; ++k) {
        const float v = read_slot(g, b + 8 + k * 12);
        if (c.type == 31) acc = acc + v;
        else if (c.type == 32) acc = acc * v;
        else if (c.type == 36) acc = acc - v >= 0 ? acc : v;
        else acc = v - acc >= 0 ? acc : v;
      }
      return c.value = acc;
    }
    case 38:  // Gradient
      return c.s[1];
    case 41: {  // Sin
      if (c.stamp == g_pass) return c.value;
      c.stamp = g_pass;
      float x = read_in(g, b + 4);
      if (x < -1) {
        const int k = (int)x - 1;
        x = x - (float)k;
      } else if (x > 1) {
        const int k = (int)x + 1;
        x = x - (float)k;
      }
      return c.value = sinf(kPi * x);
    }
    case 46:  // Envelope
      return c.s[0];
    case 48: {  // GateSwitch
      if (c.stamp == g_pass) return c.value;
      c.stamp = g_pass;
      return c.value = c.flag[0] ? read_slot(g, b + 0x30) : read_slot(g, b + 0x24);
    }
    case 49: {  // Array
      if (c.stamp == g_pass) return c.value;
      c.stamp = g_pass;
      const u32 n = be32(b + 0x18);
      auto e = [&](u32 k) { return read_slot(g, b + 0x1C + (k < n ? k : n - 1) * 12); };
      const float i = read_in(g, b + 4) - read_slot(g, b + 0xC);
      if (!n) return c.value = 0;
      if (i < 0) {
        c.value = e(0);
      } else if (i >= (float)n) {
        c.value = e(n - 1);
      } else {
        const u32 k = (u32)i;
        const float f = i - (float)k;
        c.value = f == 0 ? e(k) : e(k + 1) * f + (1 - f) * e(k);
      }
      return c.value;
    }
    case 52:
    case 53:
    case 56:
    case 57:
      return out == 1 ? c.s[1] : c.s[0];
    default:
      return 0;
  }
}

void fire(Graph& g, i32 idx) {
  if (idx < 0 || (u32)idx >= g.count) return;
  Comp& c = g.c[idx];
  if (c.type == 169) c.running = true;
}

void handle_action(Graph& g, i32 idx, u32 action) {
  if (idx < 0 || (u32)idx >= g.count) return;
  Comp& c = g.c[idx];
  const u8* b = c.b;
  switch (c.type) {
    case 52:
    case 53:
    case 56:
    case 57:
      sound_action(g, c, action);
      break;
    case 26:  // Timer
      if (action == 0) c.flag[0] = 1;
      else if (action == 1) c.flag[0] = 0;
      else if (action == 2) {
        c.s[0] = read_slot(g, b + 4);
        c.flag[1] = 1;
      } else if (action == 3) {
        c.s[0] = 0;
        c.flag[1] = 1;
      }
      break;
    case 29: {  // Random
      const float lo = read_slot(g, b + 4), hi = read_slot(g, b + 0x10);
      if (action == 0) {
        const float u = (float)next_random();
        c.s[0] = (hi - lo) * u * 2.3283064e-10f + lo;
      } else if (action == 1) {
        const int ilo = (int)(lo + 1.0f - 0.0001f), ihi = (int)hi, n = ihi - ilo + 1;
        const float u = (float)next_random();
        c.s[0] = (float)(ilo + (int)((float)n * u * 2.3283064e-10f));
      } else if (action == 2) {
        if (c.s[0] < lo) c.s[0] = lo;
        const int r = (int)(hi - lo), n = r + 1;
        if (n <= 0) break;
        const float u = (float)next_random();
        const int k = (int)((float)r * u * 2.3283064e-10f);
        const int m = (int)((float)(1 + k) + (c.s[0] - lo));
        c.s[0] = (float)(m % n) + lo;
      }
      break;
    }
    case 170: {  // Selector
      const u32 m = be32(b + 4);
      const u8* el = b + 0x14;
      auto reset = [&]() {
        for (u32 i = 0; i < m && i < 15; ++i) c.s[1 + i] = read_slot(g, el + i * 48 + 0xC);
      };
      if (action == 1) {
        reset();
        break;
      }
      if (action != 0 || !m) break;
      float total = 0;
      for (u32 i = 0; i < m && i < 15; ++i) total += c.s[1 + i];
      float thr = read_slot(g, b + 8);
      if (!(thr >= 0)) thr = 0;
      if (!(total > thr)) {
        reset();
        total = 0;
      }
      float r = ((float)next_random() * total) * 2.3283064e-10f;
      u32 sel = 0;
      while (sel < m && sel < 15 && r > c.s[1 + sel]) {
        r -= c.s[1 + sel];
        ++sel;
      }
      if (sel >= m || sel >= 15) sel = (m < 15 ? m : 15) - 1;
      c.s[0] = read_slot(g, el + sel * 48);
      c.s[1 + sel] *= read_slot(g, el + sel * 48 + 0x18);
      const float inc = read_slot(g, el + sel * 48 + 0x24);
      for (u32 j = (sel + 1) % m; j != sel; j = (j + 1) % m)
        if (j < 15) c.s[1 + j] += inc;
      break;
    }
    case 169:
      if (action == 0) c.running = true;
      break;
  }
}

void script_run(Graph& g, Comp& c) {
  const u32 n = be32(c.b + 8);
  const u8* code = c.b + 0xC;
  auto word = [&](u32 i) { return be32(code + i * 4); };
  auto stop = [&]() {
    c.running = false;
    c.pc = c.sp = 0;
    c.eq = c.gt = c.lt = false;
  };
  for (u32 guard = 0; c.running && guard < 100000; ++guard) {
    if (c.pc >= n) {
      stop();
      break;
    }
    const u32 op = word(c.pc++);
    auto operand = [&]() { return word(c.pc++); };
    auto top = [&]() -> float& { return c.stack[c.sp ? c.sp - 1 : 0]; };
    switch (op) {
      case 0x1000: {
        const u32 comp = operand(), a = operand();
        handle_action(g, (i32)comp, a);
        break;
      }
      case 0x1001: {
        const u32 comp = operand();
        if (c.sp < 16) c.stack[c.sp++] = get_value(g, (i32)comp, 0);
        break;
      }
      case 0x1002: {
        const u32 comp = operand();
        if (c.sp) --c.sp;
        if (comp < g.count && g.c[comp].type == 18) g.c[comp].s[0] = c.stack[c.sp];
        break;
      }
      case 0x1003:
        if (c.sp) --c.sp;
        break;
      case 0x1004: {
        const u32 w = operand();
        float f;
        memcpy(&f, &w, 4);
        if (c.sp < 16) c.stack[c.sp++] = f;
        break;
      }
      case 0x1100:
        c.pc = operand();
        break;
      case 0x1101:
      case 0x1102:
      case 0x1103:
      case 0x1104:
      case 0x1105:
      case 0x1106: {
        const bool take = op == 0x1101 ? c.eq : op == 0x1102 ? !c.eq : op == 0x1103 ? c.lt
                          : op == 0x1104 ? !c.lt : op == 0x1105 ? c.gt : !c.gt;
        const u32 t = word(c.pc);
        if (take) c.pc = t;
        else ++c.pc;
        break;
      }
      case 0x1200:
        fire(g, (i32)operand());
        break;
      case 0x1202:
      case 0x1203:
        stop();
        break;
      case 0x1300:
        if (c.sp >= 2) {
          const float a = c.stack[c.sp - 2], b2 = c.stack[c.sp - 1];
          c.eq = fabsf(a - b2) < 1e-4f;
          c.lt = a < b2;
          c.gt = a > b2;
        }
        break;
      case 0x1301:
        top() += get_value(g, (i32)operand(), 0);
        break;
      case 0x1302:
        top() -= get_value(g, (i32)operand(), 0);
        break;
      case 0x1303:
        top() *= get_value(g, (i32)operand(), 0);
        break;
      case 0x1304:
        top() /= get_value(g, (i32)operand(), 0);
        break;
      case 0x1305:
      case 0x1306:
      case 0x1307:
      case 0x1308: {
        const u32 w = operand();
        float f;
        memcpy(&f, &w, 4);
        if (op == 0x1305) top() += f;
        else if (op == 0x1306) top() = top() - f;
        else if (op == 0x1307) top() *= f;
        else top() /= f;
        break;
      }
      default:
        if (op == 0) {
          c.pc = c.sp = 0;
          c.eq = c.gt = c.lt = false;
        }
        break;
    }
  }
}

// Control update of the stateful components (not Scripts).
void control_update(Graph& g, Comp& c) {
  const u8* b = c.b;
  switch (c.type) {
    case 1:
      c.s[0] = read_slot(g, b + 4);
      break;
    case 24: {  // control LowPass
      const float fc = read_slot(g, b + 0xC);
      if (fc != c.s[4]) {
        const float w = kDt * kPi * fc;
        const float gg = atanf(w);
        c.s[1] = -((gg - 1) / (gg + 1));
        c.s[0] = (1 - c.s[1]) * 0.5f;
        c.s[4] = fc;
      }
      const float x = read_in(g, b + 4);
      c.s[3] = (c.s[2] + x) * c.s[0] + c.s[1] * c.s[3];
      c.s[2] = x;
      break;
    }
    case 26: {  // Timer
      if (!c.flag[0]) break;
      c.s[0] = kDt * read_slot(g, b + 0x10) + c.s[0];
      const i32 alarm = bes(b + 0x28);
      if (c.flag[1] && alarm != -1 && !(c.s[0] < read_slot(g, b + 0x1C))) {
        c.flag[1] = 0;
        fire(g, alarm);
      }
      break;
    }
    case 38: {
      const float x = read_in(g, b + 4);
      c.s[1] = ((x - c.s[0]) * (float)kDivisor) * 48000.0f * 0.00390625f;
      c.s[0] = x;
      break;
    }
    case 46: {  // Envelope
      const float tgt = read_in(g, b + 4);
      if (c.s[0] < tgt) {
        const float a = read_slot(g, b + 0xC), dd = a - 1e-4f >= 0 ? a : 1e-4f;
        c.s[0] = (1.0f / dd) * kDt + c.s[0];
        if (!(c.s[0] < tgt)) {
          c.s[0] = tgt;
          fire(g, bes(b + 0x30));
          c.s[1] = read_slot(g, b + 0x18);
        }
      } else if (c.s[0] > tgt) {
        if (c.s[1] > 0) {
          c.s[1] -= kDt;
          if (!(c.s[1] > 0)) {
            c.s[1] = 0;
            fire(g, bes(b + 0x34));
          }
        } else {
          const float r = read_slot(g, b + 0x24), dd = r - 1e-4f >= 0 ? r : 1e-4f;
          c.s[0] = c.s[0] - (1.0f / dd) * kDt;
          if (!(c.s[0] >= tgt)) {
            c.s[0] = tgt;
            fire(g, bes(b + 0x38));
          }
        }
      }
      break;
    }
    case 48: {  // GateSwitch
      const float v = read_in(g, b + 4), th = read_slot(g, b + 0xC);
      float h = read_slot(g, b + 0x18);
      if (h < 0) h = -h;
      if (!c.flag[0]) {
        if (v > th + h) {
          c.flag[0] = 1;
          c.stamp = 0xFFFFFFFF;
          fire(g, bes(b + 0x3C));
        }
      } else if (v < th - h) {
        c.flag[0] = 0;
        c.stamp = 0xFFFFFFFF;
        fire(g, bes(b + 0x40));
      }
      break;
    }
    case 52:
    case 53:
    case 56:
    case 57:
      sound_update(g, c);  // (only while playing; starting one updates it)
      break;
    default:  // mixers and effects take their settings when they run
      break;
  }
}

// Audio of what an output port takes (pulled); true if it produced sound.
bool process_audio(Graph& g, Comp& c) {
  switch (c.kind) {
    case kAudioMixer:
      return compute(g, c);
    case kAudioSound:
    case kAudioInput:
      return live(g, c) && source_ready(g, c, 1.0f);
    default:
      return false;
  }
}

// --- loading --------------------------------------------------------------------------
bool graph_load(Graph& g, const char* file, const char* name) {
  char path[96];
  snprintf(path, sizeof(path), GW_AUDIO_DIR "%s", file);
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    gw_log("sound: %s missing\n", path);
    return false;
  }
  const DWORD size = GetFileSize(f, nullptr);
  g.file = (u8*)arena(size + 64);
  DWORD done = 0;
  ReadFile(f, g.file, size, &done, nullptr);
  CloseHandle(f);
  if (be32(g.file) != 0x414F424A) {  // 'AOBJ'
    gw_log("sound: %s is not a sound graph\n", path);
    return false;
  }
  strncpy(g.name, name, sizeof(g.name) - 1);
  g.strings = (const char*)g.file + be32(g.file + 8);
  g.count = be32(g.file + 0x10);
  g.c = arena_new<Comp>(g.count);
  const u8* p = g.file + 0x14;
  for (u32 i = 0; i < g.count; ++i) {
    Comp& c = g.c[i];
    c.type = be32(p);
    c.b = p + 4;
    c.name = g.strings + be32(c.b);
    const u32 bs = body_size(c.b, c.type);
    if (!bs) {
      gw_log("sound: %s: component %u has unknown type %u\n", name, i, c.type);
      return false;
    }
    p += 4 + bs;
  }
  // Initial state.
  for (u32 i = 0; i < g.count; ++i) {
    Comp& c = g.c[i];
    const u8* b = c.b;
    u32 h, ic, oc;
    switch (c.type) {
      case 0:
      case 18:
        c.s[0] = bef(b + 4);
        break;
      case 1:
        c.s[0] = bef(b + 4 + 8);
        break;
      case 24:
        c.s[4] = -1;
        break;
      case 26:
        c.flag[0] = 0;
        c.flag[1] = 1;
        c.s[0] = read_slot(g, b + 4);
        break;
      case 170:
        handle_action(g, (i32)i, 1);
        break;
      case 12:
      case 184:
        c.kind = kAudioInput;
        c.channels = c.type == 12 ? 1 : 2;  // surround carried as stereo
        c.ext = (float*)arena(c.channels * kPass * 4);
        c.ext_next = (float*)arena(c.channels * kPass * 4);
        break;
      case 13:
      case 185:
        c.kind = kAudioOutput;
        c.channels = c.type == 13 ? 1 : 2;
        break;
      default:
        if (is_sound(c.type)) sound_init(g, c);
        else if (mixer_shape(c.type, &h, &ic, &oc)) mixer_init(g, c);
        else effect_init(g, c);
        break;
    }
  }
  // Audio buffers for the components that carry audio (an input's is its
  // external sum).
  // A mixer needs one only if it has insert effects or an output port takes
  // it: the others are folded into the mixes below them (see collect).
  for (u32 i = 0; i < g.count; ++i) {
    Comp& c = g.c[i];
    if (c.kind == kAudioMixer) c.inserts = has_inserts(g, c);
  }
  for (u32 i = 0; i < g.count; ++i) {
    Comp& c = g.c[i];
    const i32 src = c.kind == kAudioOutput ? bes(c.b + 4) : -1;
    if (src >= 0 && (u32)src < g.count) g.c[src].pulled = true;
  }
  for (u32 i = 0; i < g.count; ++i) {
    Comp& c = g.c[i];
    if (c.kind == kAudioInput) continue;  // (in_now)
    else if (c.kind == kAudioMixer && !c.inserts && !c.pulled) continue;
    else if (c.channels) c.buf = (float(*)[kPass])arena(c.channels * kPass * 4);
  }
  for (u32 i = 0; i < g.count; ++i) {
    if (g.c[i].kind == kAudioSound) ++g.sound_count;
    if (g.c[i].kind == kAudioOutput) ++g.output_count;
  }
  {
    auto has_control = [](const Comp& c) {
      return c.kind == kAudioSound || c.type == 1 || c.type == 24 || c.type == 26 || c.type == 38 ||
             c.type == 46 || c.type == 48;
    };
    u32 nscript = 0, nctl = 0, nparents = 0;
    for (u32 i = 0; i < g.count; ++i) {
      g.c[i].index = i;
      if (g.c[i].type == 169) ++nscript;
      if (has_control(g.c[i])) ++nctl;
      if (g.c[i].kind == kAudioMixer) nparents += g.c[i].nB;
    }
    g.scripts = (u32*)arena((nscript + 1) * 4);
    g.ctl = (u32*)arena((nctl + 1) * 4);
    g.playing = (u8*)arena(g.count + 1);
    g.live_pass = (u32*)arena(g.count * 4 + 4);
    g.parent_first = (u32*)arena((g.count + 2) * 4);
    g.parents = (u32*)arena((nparents + 1) * 4);
    if (g.scripts && g.ctl && g.playing && g.live_pass && g.parent_first && g.parents) {
      for (u32 i = 0; i < g.count; ++i) {
        g.live_pass[i] = 0xFFFFFFFF;
        if (g.c[i].type == 169) g.scripts[g.script_count++] = i;
        if (has_control(g.c[i])) g.ctl[g.ctl_count++] = i;
      }
      // Each component's consumers (the mixers it feeds), grouped by component.
      u32* counts = g.parent_first;
      for (u32 i = 0; i < g.count; ++i) {
        const Comp& m = g.c[i];
        if (m.kind != kAudioMixer) continue;
        u32 h, ic, oc;
        mixer_shape(m.type, &h, &ic, &oc);
        for (u32 e = 0; e < m.nB; ++e) {
          const i32 src = bes(m.b + h + 4 * m.nA + e * 16);
          if (src >= 0 && (u32)src < g.count) ++counts[src + 1];
        }
      }
      for (u32 i = 0; i < g.count; ++i) counts[i + 1] += counts[i];
      u32* fill = (u32*)arena((g.count + 1) * 4);
      if (fill) {
        memcpy(fill, g.parent_first, g.count * 4);
        for (u32 i = 0; i < g.count; ++i) {
          const Comp& m = g.c[i];
          if (m.kind != kAudioMixer) continue;
          u32 h, ic, oc;
          mixer_shape(m.type, &h, &ic, &oc);
          for (u32 e = 0; e < m.nB; ++e) {
            const i32 src = bes(m.b + h + 4 * m.nA + e * 16);
            if (src >= 0 && (u32)src < g.count) g.parents[fill[src]++] = i;
          }
        }
      }
    }
  }
  g.sounds = (u32*)arena((g.sound_count + 1) * 4);
  g.outputs = (u32*)arena((g.output_count + 1) * 4);
  if (g.sounds && g.outputs) {
    u32 ns = 0, no = 0;
    for (u32 i = 0; i < g.count; ++i) {
      if (g.c[i].kind == kAudioSound) g.sounds[ns++] = i;
      if (g.c[i].kind == kAudioOutput) g.outputs[no++] = i;
    }
  }
  if (g_arena_failed) {  // something of it has no memory: left out
    gw_log("sound: graph %s left out (no memory)\n", name);
    return false;
  }
  g.live = true;
  gw_log("sound: graph %s, %u components\n", name, g.count);
  return true;
}

Comp* find_comp(Graph& g, const char* name, u32 type) {
  for (u32 i = 0; i < g.count; ++i)
    if (g.c[i].type == type && !strcmp(g.c[i].name, name)) return &g.c[i];
  return nullptr;
}

// One audio pass of every graph; the System graph's 6 channels out.
const float* g_out[2];  // System's output this pass (null: silent)
const float* g_wet;     // its reverb send


// --- sounds on the APU (GW_SOUND_APU) ----------------------------------------------------
// A sound whose every way out is plain mixing (mixers without inserts) up to
// "bin mixers" (a graph output's mixer, or any mixer with inserts) plays on
// the APU's voice processor: a hardware voice per wave channel decodes,
// pitches and mixes it into the bins of those mixers, with the gains the
// graph's mixers give it each pass. The GP copies the bins to the ring; each
// pass reads them back and adds them into their mixers before the mixers'
// inserts run, so a mixer's effects work on what the APU and software mixed
// together. Software mixing goes on for everything else. The audio thread
// waits on the APU's output, so the two parts stay in step. A bin mixer has
// bins only while voices mix into it; a voice that finds no bins or no free
// hardware voice plays in software as before.
#ifdef GW_SOUND_APU
bool g_apu_on;
constexpr u32 kHwFirst = 64;             // xemu runs voices 0-63 through its HRTF stage (silent without an entry)
constexpr u32 kBinsUsed = kBinsCopied - 1;  // the last copied bin is where muted outputs go
constexpr u8 kBinMuted = kBinsCopied - 1;
constexpr float kHeadroom = 0.25f;       // the bins are 16-bit: gains go in at a quarter, read back times 4
constexpr u32 kTailBlocks = 16;          // silent blocks after each wave: a one-shot loops there until it is taken off
constexpr u32 kHwRings = 32;             // streams on the APU at once
constexpr u32 kRingBlocks = 256;         // a stream's ring: 256 blocks (16384 samples, 0.34 s)
u8* g_ring_mem;                          // kHwRings rings of kRingBlocks * 72 bytes
u32 g_ring_ba;                           // their offset in the sample space
bool g_ring_used[kHwRings];
struct HwOwner {
  Graph* g;
  Comp* c;
  Voice* v;
};
HwOwner g_hw[kMaxVoices];
Comp* g_bin_owner[kBinsCopied];
float g_bins[kBinsCopied][kPass];
u8 g_bins_wanted[kBinsCopied];
// A bin with sound in it this pass. A mixer whose bins are all silent is as
// a software mixer nothing reached: its inserts (and what it feeds, such as
// the reverb) are not run. Voices hold bins on every mixer they may reach,
// muted or not, and one-shots play a silent tail, so most bins are often
// silent.
bool g_bin_live[kBinsCopied];
u32 g_hw_starts, g_hw_full, g_hw_no_bins, g_apu_waves, g_apu_able, g_apu_sounds;
float g_bin_scale = 4.0f / 32768.0f;  // a bin's 16-bit value to the engine's level (apu_calibrate)

// What the APU's path does to a level, measured: a constant 0.25 (16-bit
// samples) looping on a voice at full volume into bins 0 and 1, read back
// from the ring. The bins' scale is set so that a voice at gain g reads back
// as g (with the headroom); xemu's mix came back at half.
void apu_calibrate() {
  i16* dc = (i16*)MmAllocateContiguousMemoryEx(4096, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
  if (!dc) return;
  for (u32 i = 0; i < 2048; ++i) dc[i] = 8192;
  GwApuVoice v = {};
  v.ba = gw_apu_map(dc, 4096);
  v.channels = 1;
  v.bits = 16;
  v.loop_end = 2048;
  v.left = v.right = 1.0f;
  gw_apu_voice_start(kMaxVoices - 1, v);
  gw_apu_voices_go();
  Sleep(60);
  gw_apu_skip(gw_apu_available());
  for (u32 t = 0; t < 100 && gw_apu_available() < kPass; ++t) Sleep(1);
  static float probe[kBinsCopied][kPass];
  u8 want[kBinsCopied] = {1, 1};
  float level = 0;
  if (gw_apu_available() >= kPass) {
    gw_apu_read_bins(probe, want, 1.0f / 32768.0f);
    for (u32 n = 0; n < kPass; ++n) level += probe[0][n];
    level /= kPass;
  }
  gw_apu_voice_stop(kMaxVoices - 1);
  Sleep(10);
  gw_apu_skip(gw_apu_available());
  if (level > 0.01f && level < 1.0f) g_bin_scale = 0.25f / level / kHeadroom / 32768.0f;
  gw_log("sound: the APU's mix of a 0.25 level reads back as %d/1000: bins scaled by %d/1000 of 4\n",
         (int)(level * 1000), (int)(g_bin_scale * 32768.0f / 4.0f * 1000));
}

// The bank's in-memory waves as mono Xbox ADPCM, each channel followed by a
// silent tail, in one block of contiguous memory mapped into the sample space.
void apu_prepare(Bank& b) {
  u32 total = 0;
  auto have_samples = [](const Wave& w) {
    if (w.xa) return w.samples;
    const u32 per = (w.block - 4 * w.channels) * 2 / w.channels + 1;
    u32 n = (w.size / w.block) * per;
    if (w.size % w.block > 4 * w.channels) n += (w.size % w.block - 4 * w.channels) * 2 / w.channels + 1;
    return n < w.samples ? n : w.samples;
  };
  for (u32 i = 0; i < b.count; ++i) {
    const Wave& w = b.waves[i];
    if (w.data && w.channels >= 1 && w.channels <= 2)
      total += (gw_xadpcm_blocks(have_samples(w)) + kTailBlocks) * 36 * w.channels;
  }
  total += kHwRings * kRingBlocks * 72;  // the streams' rings, after the waves
  u8* mem = total ? (u8*)MmAllocateContiguousMemoryEx(total, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE)
                  : nullptr;
  if (!mem) {
    gw_log("sound: no memory for the APU's copy of bank %s (%u KiB)\n", b.name, total / 1024);
    return;
  }
  const u32 base = gw_apu_map(mem, total);
  if (base == 0xFFFFFFFF) {
    gw_log("sound: no sample space for bank %s\n", b.name);
    return;
  }
  u32 at = 0, n = 0;
  for (u32 i = 0; i < b.count; ++i) {
    Wave& w = b.waves[i];
    if (!w.data || w.channels < 1 || w.channels > 2) continue;
    const u32 samples = have_samples(w), bytes = (gw_xadpcm_blocks(samples) + kTailBlocks) * 36 * w.channels;
    memset(mem + at, 0, bytes);
    if (w.xa) memcpy(mem + at, w.data, gw_xadpcm_blocks(samples) * 36 * w.channels);
    else gw_xadpcm_reblock(w.data, w.block, w.channels, samples, mem + at);
    w.apu_ba[0] = base + at;
    w.apu_samples = samples;
    at += bytes;
    ++n;
    ++g_apu_waves;
  }
  memset(mem + at, 0, kHwRings * kRingBlocks * 72);
  g_ring_mem = mem + at;
  g_ring_ba = base + at;
  gw_log("sound: bank %s: %u waves for the APU (%u KiB)\n", b.name, n, total / 1024);
}

// The bin mixers a sound reaches through plain mixing; false when one of its
// ways out reaches anything else.
bool apu_reach(Graph& g, u32 i, u32 depth, Comp& sound) {
  if (depth > 16 || g.parent_first[i] == g.parent_first[i + 1]) return false;
  for (u32 k = g.parent_first[i]; k < g.parent_first[i + 1]; ++k) {
    Comp& m = g.c[g.parents[k]];
    if (m.kind != kAudioMixer) return false;
    if (m.apu_out) {
      if (m.out_ch > 4) return false;
      u32 t = 0;
      while (t < sound.apu_target_n && sound.apu_targets[t] != &m) ++t;
      if (t == sound.apu_target_n) {
        if (t == 4) return false;
        sound.apu_targets[sound.apu_target_n++] = &m;
      }
    } else if (!apu_reach(g, g.parents[k], depth + 1, sound)) {
      return false;
    }
  }
  return true;
}

void apu_setup() {
  for (u32 k = 1; k < kGraphs; ++k) {
    Graph& g = g_graphs[k];
    if (!g.live) continue;
    for (u32 i = 0; i < g.count; ++i)
      if (g.c[i].kind == kAudioMixer && g.c[i].inserts) g.c[i].apu_out = true;
    for (u32 o = 0; o < g.output_count; ++o) {
      Comp& out = g.c[g.outputs[o]];
      const i32 src = bes(out.b + 4);
      if (out.dest && src >= 0 && g.c[src].kind == kAudioMixer) g.c[src].apu_out = true;
    }
    u32 sounds = 0, able = 0;
    for (u32 i = 0; i < g.count; ++i) {
      Comp& c = g.c[i];
      if (c.kind != kAudioSound) continue;
      ++sounds;
      c.apu_ok = apu_reach(g, i, 0, c) && c.apu_target_n;
      if (!c.apu_ok) c.apu_target_n = 0;
      able += c.apu_ok;
    }
    gw_log("sound: graph %s: %u of %u sounds can play on the APU\n", g.name, able, sounds);
    g_apu_able += able;
    g_apu_sounds += sounds;
  }
}

// Bins for a voice's bin mixers (each mixer's channels, held while voices
// mix into it); false (nothing taken) when there are not enough free.
bool bins_take(Comp& c) {
  u32 need = 0, free_bins = 0;
  for (u32 t = 0; t < c.apu_target_n; ++t)
    if (!c.apu_targets[t]->apu_refs) need += c.apu_targets[t]->out_ch;
  for (u32 b = 0; b < kBinsUsed; ++b) free_bins += g_bin_owner[b] == nullptr;
  if (need > free_bins) return false;
  for (u32 t = 0; t < c.apu_target_n; ++t) {
    Comp& m = *c.apu_targets[t];
    if (m.apu_refs++) continue;
    for (u32 o = 0; o < m.out_ch; ++o)
      for (u32 b = 0; b < kBinsUsed; ++b)
        if (!g_bin_owner[b]) {
          g_bin_owner[b] = &m;
          m.apu_bins[o] = (i8)b;
          break;
        }
  }
  return true;
}

void bins_give_back(Comp& c) {
  for (u32 t = 0; t < c.apu_target_n; ++t) {
    Comp& m = *c.apu_targets[t];
    if (!m.apu_refs || --m.apu_refs) continue;
    for (u32 o = 0; o < 4; ++o) {
      if (m.apu_bins[o] >= 0) g_bin_owner[m.apu_bins[o]] = nullptr;
      m.apu_bins[o] = -1;
    }
  }
}

void hw_voice_stop(Voice& v) {
  const i16 h = v.hw[0];
  if (h >= 0) {
    gw_apu_voice_stop((u32)h);
    Comp* c = g_hw[h].c;
    g_hw[h] = {};
    if (c) bins_give_back(*c);
  }
  if (v.hw_ring >= 0) g_ring_used[v.hw_ring] = false;
  v.hw[0] = v.hw[1] = -1;
  v.hw_ring = -1;
  v.rt_n[0] = v.rt_n[1] = 0;
}

// A streamed voice's ring filled up to two blocks short of its play
// position, block by block from the stream (the wave's blocks in order,
// from the start again when it loops; silence past a one-shot's end). A
// block not read from the disc yet waits, unless the voice is about to play
// it: then it is silence there, and the wave keeps its place.
void hw_ring_fill(Voice& v) {
  const Wave& w = *v.r.wave;
  const u32 ch = w.channels, bytes = 36 * ch, group = w.block / bytes;
  const u32 wave_blocks = gw_xadpcm_blocks(w.samples);
  const u32 played_blocks = (v.hw_start % 64 + v.hw_played) / 64;
  u8* ring = g_ring_mem + v.hw_ring * kRingBlocks * 72;
  while (v.hw_out < played_blocks + kRingBlocks - 2) {
    u8* dst = ring + (v.hw_out % kRingBlocks) * bytes;
    u32 wb = v.hw_start / 64 + v.hw_out;
    if (v.r.loop) wb %= wave_blocks;
    if (wb >= wave_blocks) {
      memset(dst, 0, bytes);
    } else {
      const u8* src = v.r.block_data(wb / group);
      if (src) {
        memcpy(dst, src + (wb % group) * bytes, bytes);
      } else if (v.hw_out < played_blocks + 8) {
        memset(dst, 0, bytes);  // (the stream is behind: silence, in place)
        ++gw_sound_late;
      } else {
        break;
      }
    }
    ++v.hw_out;
  }
}

void hw_voice_start(Graph& g, Comp& c, Voice& v) {
  if (!g_apu_on || !c.apu_ok || !v.playing) return;
  const Wave* w = v.r.wave;
  if (!w) return;
  const bool stream = !w->data;
  // In memory: its copy on the APU. Streamed: Xbox ADPCM, and (as the ring
  // goes block by block) a looping wave a whole number of blocks long.
  if (stream ? !w->xa || (v.r.loop && w->samples % 64) || !g_ring_mem : !w->apu_samples) return;
  hw_voice_stop(v);
  i16 h = -1;
  for (u32 k = kHwFirst; k < kMaxVoices && h < 0; ++k)
    if (!g_hw[k].v && gw_apu_voice_free(k)) h = (i16)k;
  i16 ring = -1;
  if (stream)
    for (u32 k = 0; k < kHwRings && ring < 0; ++k)
      if (!g_ring_used[k]) ring = (i16)k;
  if (h < 0 || (stream && ring < 0)) {
    ++g_hw_full;
    return;  // (no hardware voice or ring free: software)
  }
  if (!bins_take(c)) {
    ++g_hw_no_bins;
    return;  // (no bins free: software)
  }
  GwApuVoice hv = {};
  hv.channels = w->channels;
  hv.bits = 4;
  if (stream) {
    g_ring_used[ring] = true;
    v.hw_ring = ring;
    v.hw_start = v.r.pos < w->samples ? v.r.pos : 0;
    v.hw_played = v.hw_cbo = v.hw_out = 0;
    memset(g_ring_mem + ring * kRingBlocks * 72, 0, kRingBlocks * 72);
    hw_ring_fill(v);
    hv.ba = g_ring_ba + ring * kRingBlocks * 72;
    hv.start = v.hw_start % 64;
    hv.loop_start = 0;
    hv.loop_end = kRingBlocks * 64;
    v.hw_cbo = hv.start;
  } else {
    const u32 samples = w->apu_samples;
    hv.ba = w->apu_ba[0];
    hv.start = v.r.pos < samples ? v.r.pos : 0;
    if (v.r.loop) {
      hv.loop_start = 0;
      hv.loop_end = samples;
    } else {  // a one-shot loops over its silent tail until hw_positions takes it off
      hv.loop_start = samples;
      hv.loop_end = samples + kTailBlocks * 64;
    }
  }
  if (!gw_apu_voice_start((u32)h, hv)) {  // (muted: the pass's routes come at its end)
    if (stream) {
      g_ring_used[ring] = false;
      v.hw_ring = -1;
    }
    bins_give_back(c);
    ++g_hw_full;
    return;
  }
  g_hw[h] = {&g, &c, &v};
  v.hw[0] = h;
  ++g_hw_starts;
  v.rt_n[0] = v.rt_n[1] = 0;
}

// The game paused or resumed: the songs' voices (graphs 3 and up, which the
// original stops) taken off the APU's list and put back, so they stop where
// they are; nothing of theirs moves meanwhile.
inline bool hw_paused(const HwOwner& o) { return g_paused && o.g >= &g_graphs[3]; }
void hw_pause_changed() {
  static bool was = false;
  if (g_paused == was) return;
  was = g_paused;
  for (u32 h = kHwFirst; h < kMaxVoices; ++h)
    if (g_hw[h].v && g_hw[h].g >= &g_graphs[3]) gw_apu_voice_pause(h, g_paused);
}

// Where the hardware voices are: the software voices follow (the songs' sync
// tracks and progress outputs read them); one-shots past their end end;
// streams' rings are filled ahead.
void hw_positions() {
  if (!g_apu_on) return;
  for (u32 h = kHwFirst; h < kMaxVoices; ++h) {
    HwOwner o = g_hw[h];
    if (!o.v || hw_paused(o)) continue;
    Voice& v = *o.v;
    const u32 pos = gw_apu_voice_position(h);
    if (v.hw_ring >= 0) {
      const u32 span = kRingBlocks * 64;
      v.hw_played += (pos % span + span - v.hw_cbo) % span;
      v.hw_cbo = pos % span;
      const u32 samples = v.r.wave->samples, at = v.hw_start + v.hw_played;
      if (!v.r.loop && at >= samples) {
        v.r.pos = samples;
        voice_end(*o.g, *o.c, v);
        continue;
      }
      v.r.pos = v.r.loop ? at % samples : at;
      hw_ring_fill(v);
    } else {
      const u32 samples = v.r.wave->apu_samples;
      if (!v.r.loop && pos >= samples) {
        v.r.pos = samples;
        voice_end(*o.g, *o.c, v);
      } else {
        v.r.pos = pos < samples ? pos : 0;
      }
    }
  }
}

// A sound's mix into a bin mixer this pass: per hardware voice and wave
// channel, the gains A (at the pass's end) to each of d's channels.
void hw_route_add(Graph& g, Comp& d, Comp& s, const float* A) {
  (void)g;
  if (!d.apu_refs) return;
  for (u32 vi = 0; vi < s.voice_count; ++vi) {
    Voice& v = s.voices[vi];
    if (v.hw[0] < 0 || !v.playing) continue;
    const u32 wch = v.r.wave->channels;
    const float vg = v.gain_target;
    for (u32 ch = 0; ch < wch; ++ch) {
      for (u32 o = 0; o < d.out_ch && o < 4; ++o) {
        if (d.apu_bins[o] < 0) continue;
        float gain = 0;
        for (u32 cc = 0; cc < s.channels && cc < 4; ++cc)
          if ((cc < wch ? cc : 0) == ch) gain += A[cc * 4 + o];
        gain *= vg;
        if (gain <= 0) continue;
        const u8 bin = (u8)d.apu_bins[o];
        u32 k = 0;
        while (k < v.rt_n[ch] && v.rt_bin[ch][k] != bin) ++k;
        if (k == v.rt_n[ch]) {
          if (k == (wch == 2 ? 4u : 8u) || k == 4) continue;
          v.rt_bin[ch][k] = bin;
          v.rt_gain[ch][k] = 0;
          ++v.rt_n[ch];
        }
        v.rt_gain[ch][k] += gain;
      }
    }
  }
}

// The pass's routes to the hardware (none: muted), with each voice's pitch:
// a mono voice's routes on outputs 0-3, a stereo voice's left channel's on
// 0 2 4 6 and right's on 1 3 5 7.
void hw_apply() {
  if (!g_apu_on) return;
  hw_pause_changed();
  for (u32 h = kHwFirst; h < kMaxVoices; ++h) {
    HwOwner o = g_hw[h];
    if (!o.v || hw_paused(o)) continue;  // (a paused song's voice keeps its routes)
    Voice& v = *o.v;
    float step = (float)v.r.wave->rate * 2.08333e-5f * v.pitch;
    step = step < 1.0f / 256 ? 1.0f / 256 : step > 4 ? 4 : step;
    u8 bins[8];
    float gains[8];
    for (u32 k = 0; k < 8; ++k) {
      bins[k] = kBinMuted;
      gains[k] = 0;
    }
    if (v.r.wave->channels == 2) {
      for (u32 ch = 0; ch < 2; ++ch)
        for (u32 k = 0; k < v.rt_n[ch]; ++k) {
          bins[2 * k + ch] = v.rt_bin[ch][k];
          gains[2 * k + ch] = v.rt_gain[ch][k] * kHeadroom;
        }
    } else {
      for (u32 k = 0; k < v.rt_n[0]; ++k) {
        bins[k] = v.rt_bin[0][k];
        gains[k] = v.rt_gain[0][k] * kHeadroom;
      }
    }
    gw_apu_voice_mix(h, gw_apu_pitch(step), 8, bins, gains);
    v.rt_n[0] = v.rt_n[1] = 0;
  }
  gw_apu_voices_go();  // (this pass's new voices, with their first routes, together)
}

// This pass's bins into a bin mixer (before its inserts); false when all
// of them were silent.
bool hw_bins_add(Graph& g, Comp& d, bool* fresh) {
  (void)g;
  bool any = false;
  for (u32 o = 0; o < d.out_ch && o < 4; ++o) {
    if (d.apu_bins[o] < 0 || !g_bin_live[d.apu_bins[o]]) continue;
    any = true;
    const float* in = g_bins[d.apu_bins[o]];
    float* out = d.buf[o];
    if (fresh[o]) {
      memcpy(out, in, kPass * sizeof(float));
      fresh[o] = false;
    } else {
      for (u32 n = 0; n < kPass; ++n) out[n] += in[n];
    }
  }
  return any;
}

// The APU's output for this pass (256 frames), waited for: the mix follows
// the APU's clock. A backlog of more than 2048 frames is skipped (xemu's APU
// runs a little fast).
void apu_read_bins() {
  if (!g_apu_on) return;
  static bool started = false;
  if (!started) {
    started = true;
    gw_apu_skip(gw_apu_available());
  }
  for (u32 b = 0; b < kBinsCopied; ++b) g_bins_wanted[b] = g_bin_owner[b] != nullptr;
  u32 waited = 0;
  const u64 wait_from = __builtin_ia32_rdtsc();
  while (gw_apu_available() < kPass && waited < 40) {
    Sleep(1);
    ++waited;
  }
  gw_sound_wait_cycles += __builtin_ia32_rdtsc() - wait_from;
  const u32 have = gw_apu_available();
  if (have < kPass) {
    static u32 told;
    if (told++ < 8) gw_log("sound: the APU's output is late (%u frames)\n", have);
    memset(g_bins, 0, sizeof(g_bins));
    memset(g_bin_live, 0, sizeof(g_bin_live));
    return;
  }
  if (have > 2048) gw_apu_skip((have - 512) / 32 * 32);
  gw_apu_read_bins(g_bins, g_bins_wanted, g_bin_scale);
  for (u32 b = 0; b < kBinsCopied; ++b) {
    bool live = false;
    if (g_bins_wanted[b])
      for (u32 n = 0; n < kPass && !live; ++n) live = g_bins[b][n] != 0.0f;
    g_bin_live[b] = live;
  }
  static DWORD last;
  if (GetTickCount() - last > 5000) {
    last = GetTickCount();
    u32 in_use = 0, bins = 0;
    for (u32 h = kHwFirst; h < kMaxVoices; ++h) in_use += g_hw[h].v != nullptr;
    for (u32 b = 0; b < kBinsUsed; ++b) bins += g_bin_owner[b] != nullptr;
    gw_log("sound: APU voices in use %u, started %u, refused %u (no voice) %u (no bins); bins in use %u; %u frames "
           "waiting (%u waves, %u of %u sounds can play there)\n",
           in_use, g_hw_starts, g_hw_full, g_hw_no_bins, bins, gw_apu_available(), g_apu_waves, g_apu_able,
           g_apu_sounds);
  }
}
#else
void hw_voice_stop(Voice&) {}
void hw_voice_start(Graph&, Comp&, Voice&) {}
void hw_positions() {}
void hw_apply() {}
#endif

void graph_pass() {
  PROF_BEGIN(hwpos);
  hw_positions();
  PROF_END(hwpos);
  // Inputs read last pass's sums (one pass of latency), then collect anew.
  Graph& sys = g_graphs[0];
  if (sys.live) {
    for (u32 i = 0; i < sys.count; ++i) {
      Comp& c = sys.c[i];
      if (c.kind != kAudioInput) continue;
      float* t = c.ext;
      c.ext = c.ext_next;
      c.ext_next = t;
      for (u32 cc = 0; cc < 2; ++cc) c.in_now[cc] = c.in_next[cc];
      c.flag[3] = c.flag[2];  // outputs that reached it (last pass)
      c.flag[2] = 0;
    }
  }
  const bool control = g_pass % kDivisor == 0;
  if (control) {
    ++g_epoch;
    g_contrib_top = 0;
  }
  PROF_BEGIN(control);
  for (Graph& g : g_graphs) {
    if (!g.live || !control || (g_paused && &g >= &g_graphs[3])) continue;
    for (u32 k = 0; k < g.script_count; ++k) {
      Comp& c = g.c[g.scripts[k]];
      if (c.running) script_run(g, c);
    }
    for (u32 k = 0; k < g.ctl_count; ++k) {
      const u32 i = g.ctl[k];
      if (g.c[i].kind != kAudioSound || g.playing[i]) control_update(g, g.c[i]);
    }
  }
  PROF_END(control);
  PROF_BEGIN(audio);
  g_out[0] = g_out[1] = g_wet = nullptr;
  for (Graph& g : g_graphs) {
    if (!g.live) continue;
    if (g_paused && &g >= &g_graphs[3]) continue;
    if (&g == &sys) {  // System: its inputs that had sound
      for (u32 i = 0; i < g.count; ++i)
        if (g.c[i].kind == kAudioInput && g.c[i].flag[3]) mark_live(g, i);
    } else if (!g.playing_count) {
      continue;
    }
    PROF_BEGIN(mark);
    for (u32 k = 0; k < g.sound_count; ++k)
      if (g.playing[g.sounds[k]]) mark_live(g, g.sounds[k]);
    PROF_END(mark);
    for (u32 o = 0; o < g.output_count; ++o) {
      Comp& c = g.c[g.outputs[o]];
#ifdef GW_SOUND_PROFILE
      static const int only = getenv("SOUND_ONLY") ? atoi(getenv("SOUND_ONLY")) : -1;
      if (only >= 0 && &g != &g_graphs[only] && &g != &sys) continue;
#endif
      const i32 src = bes(c.b + 4);
      if (src < 0 || !process_audio(g, g.c[src])) continue;
      Comp& s = g.c[src];
      if (c.dest) {  // into System's input (next pass)
        Comp& d = *c.dest;
        const u32 writers = d.flag[2];
        d.flag[2] = writers < 255 ? writers + 1 : 255;
        for (u32 cc = 0; cc < d.channels; ++cc) {
          const float* in = s.buf[cc < s.channels ? cc : 0];
          if (!writers) {
            d.in_next[cc] = in;  // the first: used where it is
            continue;
          }
          float* sum = d.ext_next + cc * kPass;  // a second: the sum, here
          if (d.in_next[cc] != sum) memcpy(sum, d.in_next[cc], kPass * sizeof(float));
          for (u32 n = 0; n < kPass; ++n) sum[n] += in[n];
          d.in_next[cc] = sum;
        }
      } else if (&g == &sys && c.type == 185) {
        for (u32 cc = 0; cc < 2; ++cc) g_out[cc] = s.buf[cc < s.channels ? cc : 0];
      } else if (&g == &sys && c.type == 13) {
        g_wet = s.buf[0];
      }
    }
  }
  PROF_END(audio);
  PROF_BEGIN(hwapply);
  hw_apply();
  PROF_END(hwapply);
  gains_pass_done();
  ++g_pass;
}

// A simple reverb for the wet send (the 360 used XAudio's built-in I3DL2
// reverb, which is not in the game): four combs and two all-passes, its
// decay from System's DecayTime and level from its Reverb (mB).
struct Reverb {
  float comb[4][2048];
  u32 len[4] = {1557, 1617, 1491, 1422}, at[4] = {};
  float filt[4] = {};
  float ap[2][600];
  u32 ap_len[2] = {556, 441}, ap_at[2] = {};
} g_reverb;

// The pass's reverb into rev (false: silent, rev untouched). Each delay
// line runs over the pass in straight runs (the same arithmetic, in the same
// order, as sample by sample). With no send for longer than the tail
// (twice the decay time) it is skipped.
u32 g_reverb_quiet;  // passes without a send
bool reverb(const float* wet, float decay_s, float* rev) {
  if (!wet) {
    if (g_reverb_quiet > (u32)((decay_s * 2 + 0.1f) * 187.5f)) return false;
    ++g_reverb_quiet;
  } else {
    g_reverb_quiet = 0;
  }
  static float last_decay = -1, fb = 0;
  if (decay_s != last_decay) {
    last_decay = decay_s;
    fb = decay_s > 0.05f ? powf(0.001f, 1500.0f / (48000.0f * decay_s)) : 0.0f;
    if (fb > 0.97f) fb = 0.97f;
  }
  static float x[kPass];
  for (u32 k = 0; k < 4; ++k) {  // this pass's stretch of each line
    const u32 at = g_reverb.at[k], len = g_reverb.len[k], first = len - at < kPass ? len - at : kPass;
    prefetch(g_reverb.comb[k] + at, first * 4);
    if (first < kPass) prefetch(g_reverb.comb[k], (kPass - first) * 4);
  }
  for (u32 k = 0; k < 2; ++k) {
    const u32 at = g_reverb.ap_at[k], len = g_reverb.ap_len[k], first = len - at < kPass ? len - at : kPass;
    prefetch(g_reverb.ap[k] + at, first * 4);
    if (first < kPass) prefetch(g_reverb.ap[k], (kPass - first) * 4);
  }
  if (wet) prefetch(wet, kPass * 4);
  for (u32 n = 0; n < kPass; ++n) x[n] = wet ? wet[n] * 0.25f : 0.0f;
  for (u32 n = 0; n < kPass; ++n) rev[n] = 0;
  for (u32 k = 0; k < 4; ++k) {
    float* line = g_reverb.comb[k];
    u32 at = g_reverb.at[k];
    const u32 len = g_reverb.len[k];
    float f = g_reverb.filt[k];
    for (u32 n = 0; n < kPass;) {
      u32 run = len - at;
      if (run > kPass - n) run = kPass - n;
      float* y = line + at;
      for (u32 i = 0; i < run; ++i) {
        const float out = y[i];
        f = out * 0.7f + f * 0.3f;
        y[i] = x[n + i] + f * fb;
        rev[n + i] += out;
      }
      n += run;
      at += run;
      if (at >= len) at = 0;
    }
    g_reverb.at[k] = at;
    g_reverb.filt[k] = f;
  }
  for (u32 k = 0; k < 2; ++k) {
    float* line = g_reverb.ap[k];
    u32 at = g_reverb.ap_at[k];
    const u32 len = g_reverb.ap_len[k];
    for (u32 n = 0; n < kPass;) {
      u32 run = len - at;
      if (run > kPass - n) run = kPass - n;
      float* b = line + at;
      for (u32 i = 0; i < run; ++i) {
        const float sv = rev[n + i];
        const float out = b[i] - sv;
        b[i] = sv + b[i] * 0.5f;
        rev[n + i] = out;
      }
      n += run;
      at += run;
      if (at >= len) at = 0;
    }
    g_reverb.ap_at[k] = at;
  }
  return true;
}

}  // namespace

// --- the engine's interface ---------------------------------------------------------------

void gw_sound_init() {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  InitializeCriticalSection(&g_file_lock);
  InitializeCriticalSection(&g_arena_lock);
  init_diffuse();
  adpcm_tables();
  g_ready = true;
}

// Reader thread: keeps each playing stream's ring filled from the disc, in
// reads of up to kReadBlocks blocks straight into the ring.
namespace {
HANDLE g_reader_wake;
constexpr u32 kHeadBlocks = 8;
// Effects of up to this size are held in memory whole; the longer ones (the
// magnet loop, start-up, game over, the smart bomb...: 34 waves, 3.7 MB of
// the bank's 4.6 MB) stream like the music.
constexpr u32 kEffectsInMemory = 32 * 1024;  // a streamed wave's first blocks held in memory (0.17 s stereo)
DWORD WINAPI reader_thread(LPVOID) {
  // The streamed waves' first blocks, the menu's first.
  static const char* const kOrder[] = {"GW3", "MenuBank", "Song1Bank", "Song2Bank", "Song3Bank",
                                       "Song4Bank", "Song5Bank", "Song6Bank"};
  u32 head_bytes = 0;
  for (const char* name : kOrder) {
    for (Bank& b : g_banks) {
      if (!b.count || _stricmp(b.name, name)) continue;
      for (u32 i = 0; i < b.count; ++i) {
        Wave& w = b.waves[i];
        if (!w.samples || w.block > kMaxBlock || w.data) continue;
        const u32 blocks = (w.size + w.block - 1) / w.block;
        const u32 n = blocks < kHeadBlocks ? blocks : kHeadBlocks;
        const u32 bytes = n == blocks ? w.size : n * w.block;
        u8* head = (u8*)arena(bytes);
        if (!head) break;
        DWORD done = 0;
        EnterCriticalSection(&g_file_lock);
        SetFilePointer(b.file, w.offset, nullptr, FILE_BEGIN);
        ReadFile(b.file, head, bytes, &done, nullptr);
        LeaveCriticalSection(&g_file_lock);
        if (done != bytes) continue;
        EnterCriticalSection(&g_lock);
        w.head_blocks = n;
        w.head = head;
        LeaveCriticalSection(&g_lock);
        head_bytes += bytes;
      }
    }
  }
  gw_log("sound: streamed waves' first blocks in memory (%u KiB)\n", head_bytes / 1024);
  for (;;) {
    WaitForSingleObject(g_reader_wake, 10);
    for (Ring& rg : g_rings) {
      {
        {
          for (int rounds = 0; rounds < 4; ++rounds) {
            EnterCriticalSection(&g_lock);
            Reader* const owner = rg.owner;
            Ring* const ring = owner && owner->wave && owner->streamed() ? &rg : nullptr;
            const u32 block = owner ? owner->ring_next : 0, slot = owner ? owner->ring_written % kStreamBlocks : 0;
            u32 count = ring ? kStreamBlocks - (owner->ring_written - owner->ring_read) : 0;
            LeaveCriticalSection(&g_lock);
            if (!count) break;
            Reader& r = *owner;
            if (!count || block >= r.blocks()) break;
            // Contiguous blocks into contiguous slots (a ring's slots are
            // its wave's block size apart).
            if (count > kReadBlocks) count = kReadBlocks;
            if (count > kStreamBlocks - slot) count = kStreamBlocks - slot;
            if (count > r.blocks() - block) count = r.blocks() - block;
            u32 bytes = 0;
            for (u32 k = 0; k < count; ++k) bytes += r.block_bytes(block + k);
            DWORD done = 0;
            EnterCriticalSection(&g_file_lock);
            SetFilePointer(r.bank->file, r.wave->offset + block * r.wave->block, nullptr, FILE_BEGIN);
            ReadFile(r.bank->file, ring->data + slot * r.wave->block, bytes, &done, nullptr);
#ifdef GW_SOUND_PROFILE
            {  // SLOW_DISC=ms: each read takes that long (the console's disc)
              static const int slow = getenv("SLOW_DISC") ? atoi(getenv("SLOW_DISC")) : 0;
              if (slow) Sleep(slow);
            }
#endif
            LeaveCriticalSection(&g_file_lock);
            EnterCriticalSection(&g_lock);
            if (r.ring == ring && r.ring_next == block && r.ring_written % kStreamBlocks == slot && done == bytes) {
              r.ring_written += count;
              r.ring_next = block + count;
            }
            LeaveCriticalSection(&g_lock);
            if (done != bytes) break;
          }
        }
      }
    }
  }
  return 0;
}
}  // namespace

// Processor timings (cycles), logged once at start-up: what a pass's kinds
// of work cost on this machine.
namespace {
void bench() {
  static float a[4][2048], x[256], acc[256];
  for (u32 i = 0; i < 2048; ++i)
    for (u32 k = 0; k < 4; ++k) a[k][i] = 0.001f * (float)(i % 7);
  for (u32 i = 0; i < 256; ++i) x[i] = 0.01f * (float)i;
  u64 best[4] = {~0ull, ~0ull, ~0ull, ~0ull};
  volatile float sink = 0;
  for (int rep = 0; rep < 5; ++rep) {
    // 1. a serial multiply-add chain, 1024 steps
    u64 t0 = __builtin_ia32_rdtsc();
    float f = 0.5f;
    for (u32 i = 0; i < 1024; ++i) f = f * 0.7f + 0.3f;
    u64 t1 = __builtin_ia32_rdtsc();
    sink = f;
    // 2. the reverb's comb work on cached data (4 lines x 256)
    for (u32 n = 0; n < 256; ++n) acc[n] = 0;
    u64 t2 = __builtin_ia32_rdtsc();
    for (u32 k = 0; k < 4; ++k) {
      float ff = 0;
      float* y = a[k] + rep * 256;
      for (u32 i = 0; i < 256; ++i) {
        const float out = y[i];
        ff = out * 0.7f + ff * 0.3f;
        y[i] = x[i] + ff * 0.9f;
        acc[i] += out;
      }
    }
    u64 t3 = __builtin_ia32_rdtsc();
    // 3. a mix: 256 multiply-adds over two buffers
    for (u32 i = 0; i < 256; ++i) acc[i] += x[i] * 0.5f;
    u64 t4 = __builtin_ia32_rdtsc();
    // 4. rdtsc itself
    u64 t5 = __builtin_ia32_rdtsc();
    const u64 d[4] = {t1 - t0, t3 - t2, t4 - t3, t5 - t4};
    for (int k = 0; k < 4; ++k) best[k] = d[k] < best[k] ? d[k] : best[k];
  }
  gw_log("sound: timing (cycles, best of 5): serial chain of 1024 %u, reverb combs %u, a 256-sample mix %u, "
         "rdtsc %u\n",
         (u32)best[0], (u32)best[1], (u32)best[2], (u32)best[3]);
  (void)sink;
}
}  // namespace

void gw_sound_start() {
  bench();
  gw_sound_init();
  static const char* const kBanksToOpen[] = {"GW3", "MenuBank", "Song1Bank", "Song2Bank",
                                             "Song3Bank", "Song4Bank", "Song5Bank", "Song6Bank"};
  for (const char* b : kBanksToOpen) bank_open(b);
  // The effects in memory (they must start at once); the music streams.
  for (Bank& b : g_banks)
    if (b.count && !_stricmp(b.name, "GW3")) bank_resident(b, kEffectsInMemory);
#ifdef GW_SOUND_APU
  g_apu_on = gw_apu_init();
  if (g_apu_on)
    for (Bank& b : g_banks)
      if (b.count && !_stricmp(b.name, "GW3")) apu_prepare(b);
#endif
#ifdef GW_SOUND_PROFILE
  if (getenv("SOUND_RESIDENT"))  // the PC check's reference: every bank in memory
    for (Bank& b : g_banks)
      if (b.count) bank_resident(b, 0xFFFFFFFF);
#endif
  for (Ring& ring : g_rings) ring.data = (u8*)arena(kStreamBlocks * kMaxBlock);
  g_reader_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  // Above the game's threads: it needs little processor time, but a busy
  // main thread (a crowd on screen, the frame over budget) left it waiting to
  // run, and the music's streams fell behind (heard as gaps).
  if (HANDLE t = CreateThread(nullptr, 64 * 1024, reader_thread, nullptr, 0, nullptr))
    SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
  // The graphs, in the game's creation order.
  static const char* const kFiles[kGraphs][2] = {
      {"System.dat", "System"}, {"GW3_FX.dat", "FX"},       {"UIMusic.dat", "UIMusic"},
      {"Song_1.dat", "Song1"},  {"Song_2.dat", "Song2"},    {"Song_3.dat", "Song3"},
      {"Song_4.dat", "Song4"},  {"Song_5.dat", "Song5"},    {"Song_6.dat", "Song6"}};
  for (u32 k = 0; k < kGraphs; ++k) graph_load(g_graphs[k], kFiles[k][0], kFiles[k][1]);
  // Connections into System.
  Graph& sys = g_graphs[0];
  auto connect = [&](Graph& g, const char* out, const char* in) {
    Comp* o = nullptr;
    for (u32 i = 0; i < g.count; ++i)
      if ((g.c[i].type == 13 || g.c[i].type == 185) && !strcmp(g.c[i].name, out)) o = &g.c[i];
    Comp* d = nullptr;
    for (u32 i = 0; i < sys.count; ++i)
      if ((sys.c[i].type == 12 || sys.c[i].type == 184) && !strcmp(sys.c[i].name, in)) d = &sys.c[i];
    if (o && d) o->dest = d;
    else gw_log("sound: no connection %s.%s -> System.%s\n", g.name, out, in);
  };
  connect(g_graphs[1], "FXOutput", "FXInput");
  connect(g_graphs[2], "UIMusicOutput", "UIMusicInput");
  connect(g_graphs[2], "MonoOutput", "UiMusicMonoInput");
  for (u32 s = 1; s <= 6; ++s) {
    char out[24], in[24];
    snprintf(out, sizeof(out), "Song%uOutput", s);
    snprintf(in, sizeof(in), "Song%uInput", s);
    connect(g_graphs[2 + s], out, in);
    snprintf(in, sizeof(in), "Song%uMonoInput", s);
    connect(g_graphs[2 + s], "MonoOutput", in);
  }
#ifdef GW_SOUND_APU
  if (g_apu_on) {
    apu_calibrate();
    apu_setup();
  }
#endif
  u32 comps = 0, voices = 0, buffers = 0;
  for (Graph& g : g_graphs) {
    comps += g.count;
    for (u32 i = 0; i < g.count; ++i) {
      voices += g.c[i].voice_count;
      buffers += g.c[i].channels;
    }
  }
  gw_log("sound: %u components, %u voices, %u audio buffers; engine memory %u KiB\n", comps, voices, buffers,
         g_arena_total / 1024);
#ifndef GW_SOUND_PROFILE
  gw_log_memory("sound engine loaded");
#endif
  gw_audio_start();
}

namespace {
const char* table_name(u32 table, u32 count, u32 id) {
  for (u32 i = 0; i < count; ++i) {
    if (gw_load32(table + i * 8) == id) {
      const u32 name = gw_load32(table + i * 8 + 4);
      return name ? (const char*)gw_host(name) : nullptr;
    }
  }
  return nullptr;
}
}  // namespace

// The game's events and parameters by number, each found by name once
// (the first time it is used) and kept: the game sets 22 parameters every
// frame, and looking each up among 1342 components by name cost the game's
// main thread several per cent of its time.
namespace {
constexpr u32 kEvents = 113, kParams = 22, kTargets = 12;
struct Targets {
  bool found;
  u32 count;
  Graph* graph[kTargets];
  Comp* comp[kTargets];
};
Targets g_event_targets[kEvents], g_param_targets[kParams];
// (g_lock held.) Every component of `type` named `name` (actions: not in
// System, which the game does not send them to).
void find_targets(Targets& t, const char* name, u32 type) {
  t.found = true;
  t.count = 0;
  for (u32 k = type == 2 ? 1 : 0; k < kGraphs; ++k) {
    Graph& g = g_graphs[k];
    if (!g.live) continue;
    for (u32 i = 0; i < g.count; ++i) {
      if (g.c[i].type != type || strcmp(g.c[i].name, name)) continue;
      if (t.count < kTargets) {
        t.graph[t.count] = &g;
        t.comp[t.count] = &g.c[i];
        ++t.count;
      }
      if (type == 2) break;  // one action of a name per graph
    }
  }
}
}  // namespace

void gw_sound_event(u32 event) {
  if (!g_ready || event >= kEvents) return;
  Targets& t = g_event_targets[event];
  EnterCriticalSection(&g_lock);
  if (!t.found) {
    const char* name = table_name(0x8218D588, kEvents, event);
    if (name) find_targets(t, name, 2);
    else t.found = true;
  }
#ifdef GW_AUDIO_CAPTURE
  {
    const char* name = table_name(0x8218D588, kEvents, event);
    gw_log("sound event %s (%u) at pass %u, %u targets\n", name ? name : "?", event, g_pass, t.count);
  }
#endif
  for (u32 k = 0; k < t.count; ++k) fire(*t.graph[k], bes(t.comp[k]->b + 4));
  LeaveCriticalSection(&g_lock);
}

void gw_sound_action(const char* name) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
#ifdef GW_AUDIO_CAPTURE
  gw_log("sound action %s at pass %u\n", name, g_pass);
#endif
  for (u32 k = 1; k < kGraphs; ++k) {  // the game sends actions to FX, UIMusic and the songs
    Graph& g = g_graphs[k];
    if (!g.live) continue;
    for (u32 i = 0; i < g.count; ++i) {
      if (g.c[i].type == 2 && !strcmp(g.c[i].name, name)) {
        fire(g, bes(g.c[i].b + 4));
        break;
      }
    }
  }
  LeaveCriticalSection(&g_lock);
}

void gw_sound_param(u32 param, float value) {
  if (!g_ready || param >= kParams) return;
  Targets& t = g_param_targets[param];
  EnterCriticalSection(&g_lock);
  if (!t.found) {
    const char* name = table_name(0x8218D918, kParams, param);
    if (name) find_targets(t, name, 0);
    else t.found = true;
  }
#ifdef GW_AUDIO_CAPTURE
  if (param == 0) {  // (Thrust: set every frame) what is playing, every 5 s
    static DWORD last_debug = GetTickCount();
    if (GetTickCount() - last_debug > 5000) {
      last_debug = GetTickCount();
      LeaveCriticalSection(&g_lock);
      gw_sound_debug();
      EnterCriticalSection(&g_lock);
    }
  }
  {
    static float last[kParams];
    static bool seen[kParams];
    if (!seen[param] || last[param] != value) {
      // (Score, the clock and the ship's position change every frame: only
      // their first value.)
      if (!seen[param] || (param != 11 && param != 13 && param != 17 && param != 18)) {
        const char* name = table_name(0x8218D918, kParams, param);
        gw_log("sound param %s (%u) = %d/1000 at pass %u\n", name ? name : "?", param, (int)(value * 1000), g_pass);
      }
      seen[param] = true;
      last[param] = value;
    }
  }
#endif
  for (u32 k = 0; k < t.count; ++k) t.comp[k]->s[0] = value;
  LeaveCriticalSection(&g_lock);
}

void gw_sound_set_param(const char* name, float value) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  for (Graph& g : g_graphs) {
    if (!g.live) continue;
    for (u32 i = 0; i < g.count; ++i)
      if (g.c[i].type == 0 && !strcmp(g.c[i].name, name)) {
#ifdef GW_AUDIO_CAPTURE
        if (g.c[i].s[0] != value)
          gw_log("sound param %s.%s %d/1000 -> %d/1000 at pass %u\n", g.name, name, (int)(g.c[i].s[0] * 1000),
                 (int)(value * 1000), g_pass);
#endif
        g.c[i].s[0] = value;
      }
  }
  LeaveCriticalSection(&g_lock);
}

float gw_sound_output(u32 output) {
  (void)output;
  if (!g_ready || !g_graphs[5].live) return 0.0f;
  EnterCriticalSection(&g_lock);
  static Comp* c = find_comp(g_graphs[5], "TransitionSwitch", 1);
  const float v = c ? c->s[0] : 0.0f;
  LeaveCriticalSection(&g_lock);
  return v;
}

void gw_sound_pause(bool paused) { g_paused = paused; }

// What is playing (the PC check's listing): each sounding player with its
// gain, and each mixer's loudest output.
void gw_sound_debug() {
  EnterCriticalSection(&g_lock);
  for (Graph& g : g_graphs) {
    if (!g.live) continue;
    for (u32 i = 0; i < g.count; ++i) {
      Comp& c = g.c[i];
      if (c.kind == kAudioSound) {
        for (u32 v = 0; v < c.voice_count; ++v) {
          const Voice& vo = c.voices[v];
          if (!vo.playing) continue;
          // (thousandths: the Xbox's printf has no %f)
          gw_log("  %s #%u %s voice %u gain %d/1000 (now %d) pitch %d/1000 pos %u/%u %s\n", g.name, i, c.name, v,
                 (int)(vo.gain_target * 1000), (int)(vo.gain_last * 1000), (int)(vo.pitch * 1000), vo.r.pos,
                 vo.r.wave ? vo.r.wave->samples : 0, vo.r.wave ? vo.r.wave->name : "?");
        }
      } else if (c.kind == kAudioMixer && c.active && c.buf) {
        float peak = 0;
        for (u32 o = 0; o < c.out_ch; ++o)
          for (u32 n = 0; n < kPass; ++n) peak = c.buf[o][n] != c.buf[o][n] ? 999.0f : fmaxf(peak, fabsf(c.buf[o][n]));
        gw_log("  %s #%u %s (mixer %u) peak %d/1000\n", g.name, i, c.name, c.type, (int)(peak * 1000));
      }
    }
  }
  LeaveCriticalSection(&g_lock);
}

namespace {
// What keeps a sound off the APU: a way out that reaches something other than
// mixers before a bin mixer (a graph output's mixer, or one with inserts).
const Comp* apu_blocker(Graph& g, u32 i, u32 depth, const bool* bin_mixer, const Comp** target) {
  if (depth > 16 || g.parent_first[i] == g.parent_first[i + 1]) return &g.c[i];
  for (u32 k = g.parent_first[i]; k < g.parent_first[i + 1]; ++k) {
    const u32 p = g.parents[k];
    const Comp& m = g.c[p];
    if (m.kind != kAudioMixer) return &m;
    if (bin_mixer[p]) {
      *target = &m;
      continue;
    }
    const Comp* b = apu_blocker(g, p, depth + 1, bin_mixer, target);
    if (b) return b;
  }
  return nullptr;
}
}  // namespace

// (The PC check, APU_REPORT=1.) Per graph, each sound: whether it could play
// on the APU (and into which bin mixer) or what keeps it in software; and
// whether its wave is held in memory.
void gw_sound_apu_report() {
  for (u32 k = 1; k < kGraphs; ++k) {
    Graph& g = g_graphs[k];
    if (!g.live) continue;
    bool* bin_mixer = (bool*)calloc(g.count, 1);
    for (u32 i = 0; i < g.count; ++i) bin_mixer[i] = g.c[i].kind == kAudioMixer && g.c[i].inserts;
    for (u32 o = 0; o < g.output_count; ++o) {
      const Comp& out = g.c[g.outputs[o]];
      const i32 src = bes(out.b + 4);
      if (out.dest && src >= 0 && g.c[src].kind == kAudioMixer) bin_mixer[src] = true;
    }
    for (u32 i = 0; i < g.count; ++i) {
      const Comp& c = g.c[i];
      if (c.kind != kAudioSound) continue;
      const Comp* target = nullptr;
      const Comp* b = apu_blocker(g, i, 0, bin_mixer, &target);
      const Wave* w = c.voices ? c.voices[0].r.wave : nullptr;
      gw_log("  %s %-32s %s %s%s\n", g.name, c.name, w && w->data ? "memory" : "stream",
             b ? "software: reaches " : "APU into ", b ? b->name : target ? target->name : "?");
    }
    free(bin_mixer);
  }
}

bool gw_sound_test_no_reverb = false;  // (the PC check: the mix without the reverb)

// A wave held in memory, by bank and name: its converted data (IMA ADPCM in
// Microsoft blocks) and layout; false if there is none or it streams.
bool gw_sound_find_wave(const char* bank, const char* name, GwSoundWave* out) {
  Bank* b = nullptr;
  const Wave* w = find_wave(bank, name, &b);
  if (!w || !w->data) return false;
  out->data = w->data;
  out->size = w->size;
  out->block = w->block;
  out->channels = w->channels;
  out->samples = w->samples;
  out->rate = w->rate;
  out->xa = w->xa;
  return true;
}

// One frame (256 samples, 48 kHz stereo) for the AC97 (src/audio.cpp).
void gw_sound_mix(i16* out, u32 samples) {
  static float rev[kPass];
  static Comp *room, *decay, *level;
  const float* dry[2] = {nullptr, nullptr};
  bool have_rev = false;
  float level_lin = 0;
  if (g_ready && samples == kPass && g_graphs[0].live) {
#ifdef GW_SOUND_APU
    const u64 wait0 = gw_sound_wait_cycles;
    PROF_BEGIN(hwread);
    apu_read_bins();
    PROF_END(hwread);
    g_prof_hwread -= gw_sound_wait_cycles - wait0;
#endif
    PROF_BEGIN(lock);
    EnterCriticalSection(&g_lock);
    PROF_END(lock);
    if (!decay) {
      room = find_comp(g_graphs[0], "ReverbRoomParameterOut", 1);
      decay = find_comp(g_graphs[0], "ReverbDecayTimeParameterOut", 1);
      level = find_comp(g_graphs[0], "ReverbParameterOut", 1);
    }
    graph_pass();
    // System's reverb settings (I3DL2: room and reverb levels in mB).
    const float decay_s = decay ? decay->s[0] : 1.0f;
    const float mb = (room ? room->s[0] : -1000.0f) + (level ? level->s[0] : 0.0f);
    static float last_mb = 1e30f, last_level = 0;
    if (mb != last_mb) {
      last_mb = mb;
      last_level = mb < -6000 ? 0.0f : powf(10.0f, mb / 2000.0f);
    }
    level_lin = last_level;
    LeaveCriticalSection(&g_lock);
    dry[0] = g_out[0];
    dry[1] = g_out[1];
    PROF_BEGIN(reverb);
    have_rev = level_lin > 0 && !gw_sound_test_no_reverb && reverb(g_wet, decay_s, rev);
    PROF_END(reverb);
  }
  // Output: the stereo fold sums up to three surround channels into each
  // side, so the mix is scaled by kOutputGain, with a soft knee above
  // kKnee instead of hard clipping.
  PROF_BEGIN(out);
  const float kOutputGain = 0.5f, kKnee = 0.8f;
  for (u32 n = 0; n < samples && n < kPass; ++n) {
    const float wet = have_rev ? rev[n] * level_lin : 0.0f;
    float v[2] = {((dry[0] ? dry[0][n] : 0.0f) + wet) * kOutputGain, ((dry[1] ? dry[1][n] : 0.0f) + wet) * kOutputGain};
    for (float& x : v) {
      const float m = fabsf(x);
      if (m > kKnee) {
        const float t = (m - kKnee) * (1.0f / (1.0f - kKnee));
        const float y = kKnee + (1.0f - kKnee) * t / (1.0f + t);
        x = x < 0 ? -y : y;
      }
    }
    out[n * 2] = (i16)(v[0] * 32767.0f);
    out[n * 2 + 1] = (i16)(v[1] * 32767.0f);
  }
  PROF_END(out);
}

