// The Xbox APU's voice processor and our GP program (src/apu.cpp).
#pragma once
#include "runtime.h"

#include <math.h>

constexpr u32 kMaxVoices = 128;  // hardware voice records (a console handles 0..127)
constexpr u32 kBins = 32;        // the VP's mix bins
constexpr u32 kBinsCopied = 16;  // bins 0-15 copied to the ring each APU frame (src/apu.cpp)

// A hardware voice: Xbox ADPCM (36-byte blocks of 64 samples per channel) at
// sample-space offset ba (gw_apu_map), looping over [loop_start, loop_end)
// in sample frames from start; gains for front left and right (0..1); pitch
// as gw_apu_pitch gives it (0: the voice's rate is 48 kHz).
struct GwApuVoice {
  u32 ba;
  u32 channels;
  u32 bits;  // 4: Xbox ADPCM; 16: 16-bit samples
  u32 start, loop_start, loop_end;
  i16 pitch;
  float left, right;
};

bool gw_apu_init();   // tables, the GP program, frames on; false if the APU did not come up
bool gw_apu_ready();
u32 gw_apu_frames();  // APU frames the GP has run (32 samples each)

// Maps physically contiguous memory into the VP's sample space (once, for
// good); its offset there, or 0xFFFFFFFF when the space is full.
u32 gw_apu_map(const void* data, u32 bytes);

// Voice h free to start: not listed, and off the list long enough that the
// VP can no longer reach it (two APU frames).
bool gw_apu_voice_free(u32 h);
bool gw_apu_voice_start(u32 h, const GwApuVoice& v);  // false: not free; plays from the next gw_apu_voices_go
void gw_apu_voices_go();  // the voices started since the last call, listed together
void gw_apu_voice_pause(u32 h, bool paused);  // off the list (its place kept) / back at the next go
void gw_apu_voice_set(u32 h, i16 pitch, float left, float right);
// Its pitch, and outputs 0..n-1 (n <= 8) into bins[k] at gains[k] (0..1),
// the rest muted.
void gw_apu_voice_mix(u32 h, i16 pitch, u32 n, const u8* bins, const float* gains);
void gw_apu_voice_loop(u32 h, u32 loop_start, u32 loop_end);
void gw_apu_voice_stop(u32 h);
u32 gw_apu_voice_position(u32 h);  // the sample frame being played
i16 gw_apu_pitch(float ratio);     // the VP's pitch (log2 in 4.12) for a rate ratio to 48 kHz

// The GP's output: frames waiting in the ring (whole APU frames of 32);
// skipping some; reading 256 frames of the bins marked in wanted[kBins]
// into bins[kBins][256] (16-bit values times scale); and (the test) up to
// frames frames of bins 0 and 1 as stereo 16-bit, how many.
u32 gw_apu_available();
void gw_apu_skip(u32 frames);
void gw_apu_read_bins(float (*bins)[256], const u8* wanted, float scale);
u32 gw_apu_read(i16* out, u32 frames);

// IMA ADPCM in Microsoft blocks of src_block bytes (the converted banks) into
// Xbox ADPCM: dst holds gw_xadpcm_blocks(samples) * 36 * channels bytes.
u32 gw_xadpcm_blocks(u32 samples);
u32 gw_xadpcm_reblock(const u8* src, u32 src_block, u32 channels, u32 samples, u8* dst);
// One channel of the source as mono Xbox ADPCM (gw_xadpcm_blocks(samples) * 36 bytes).
u32 gw_xadpcm_reblock_mono(const u8* src, u32 src_block, u32 src_channels, u32 channel, u32 samples, u8* dst);
// The same source decoded to 16-bit samples (interleaved).
void gw_ima_decode(const u8* src, u32 src_block, u32 channels, u32 samples, i16* dst);
