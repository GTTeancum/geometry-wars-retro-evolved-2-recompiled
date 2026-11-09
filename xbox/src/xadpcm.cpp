// IMA ADPCM in Microsoft blocks (the converted banks) into Xbox ADPCM, the
// format the APU's voice processor decodes (src/apu.cpp). No hardware here:
// the PC check (tools/sound_check) tests it too.
#include "apu.h"

#include <string.h>

namespace {
const int kSteps[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
                        25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
                        88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
                        307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
                        1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
                        3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
                        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
}  // namespace

u32 gw_xadpcm_blocks(u32 samples) { return (samples + 63) / 64; }

namespace {
// One IMA ADPCM code from its decoder state (pred, idx); the state moves on.
int ima_decode(int& pred, int& idx, int nib) {
  const int step = kSteps[idx];
  int diff = step >> 3;
  if (nib & 1) diff += step >> 2;
  if (nib & 2) diff += step >> 1;
  if (nib & 4) diff += step;
  pred += nib & 8 ? -diff : diff;
  pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
  idx += kIndex[nib];
  idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
  return pred;
}

// The code that brings the state (pred, idx) nearest to target, of all 16;
// among equally near ones, the one whose step index comes nearest to
// target_idx (the source decoder's), so the two decoders meet again sooner.
int ima_encode(int pred, int idx, int target, int target_idx) {
  int best = 0, best_err = 1 << 30, best_idx_gap = 1 << 30;
  for (int nib = 0; nib < 16; ++nib) {
    int p = pred, i = idx;
    ima_decode(p, i, nib);
    const int err = p > target ? p - target : target - p;
    const int gap = i > target_idx ? i - target_idx : target_idx - i;
    if (err < best_err || (err == best_err && gap < best_idx_gap)) {
      best = nib;
      best_err = err;
      best_idx_gap = gap;
    }
  }
  return best;
}
}  // namespace

namespace {
// Channel c of the source (src_channels interleaved) into channel dc of dst
// (dst_channels interleaved, Xbox ADPCM, zeroed beforehand).
void reblock_channel(const u8* src, u32 src_block, u32 src_channels, u32 c, u32 samples, u8* dst, u32 dst_channels,
                     u32 dc) {
  const u32 src_per = (src_block - 4 * src_channels) * 2 / src_channels + 1;
  int pred = 0, idx = 0, opred = 0, oidx = 0;
  for (u32 s = 0; s < samples; ++s) {
    const u32 b = s / src_per, k = s % src_per;
    const u8* blk = src + b * src_block;
    int nib = -1;
    const bool same = pred == opred && idx == oidx;
    if (k == 0) {
      pred = (i16)(blk[c * 4] | blk[c * 4 + 1] << 8);
      idx = blk[c * 4 + 2] > 88 ? 88 : blk[c * 4 + 2];
    } else {
      const u32 code = k - 1;  // codes after the header, 8 per 4-byte group per channel
      const u8* q = blk + 4 * src_channels + (code / 8) * 4 * src_channels + 4 * c;
      nib = (q[(code % 8) / 2] >> ((code & 1) * 4)) & 15;
      ima_decode(pred, idx, nib);
    }
    const u32 ob = s / 64, ok = s % 64;
    u8* o = dst + ob * 36 * dst_channels;
    if (ok == 0) {
      o[4 * dc + 0] = (u8)(pred & 0xFF);
      o[4 * dc + 1] = (u8)((pred >> 8) & 0xFF);
      o[4 * dc + 2] = (u8)idx;
      o[4 * dc + 3] = 0;
      opred = pred;
      oidx = idx;
      continue;
    }
    const int out = nib >= 0 && same ? nib : ima_encode(opred, oidx, pred, idx);
    ima_decode(opred, oidx, out);
    const u32 oc = ok - 1;
    u8* oq = o + 4 * dst_channels + (oc / 8) * 4 * dst_channels + 4 * dc;
    oq[(oc % 8) / 2] |= (u8)(out << ((oc & 1) * 4));
  }
}
}  // namespace

// Two decoders per channel: the source's, and the one the VP will run on the
// output. Every 64th sample starts a 36-byte block whose header is that
// sample and the source's step index; the next 63 samples' codes follow (the
// 64th code is not played). While both decoders agree the source's code is
// copied (exact); where they cannot (the source starting a block of its own
// between two of ours) the source's sample is encoded from the output
// decoder's state, until our next header joins them again.
u32 gw_xadpcm_reblock(const u8* src, u32 src_block, u32 channels, u32 samples, u8* dst) {
  memset(dst, 0, gw_xadpcm_blocks(samples) * 36 * channels);
  for (u32 c = 0; c < channels; ++c) reblock_channel(src, src_block, channels, c, samples, dst, channels, c);
  return gw_xadpcm_blocks(samples) * 36 * channels;
}

u32 gw_xadpcm_reblock_mono(const u8* src, u32 src_block, u32 src_channels, u32 channel, u32 samples, u8* dst) {
  memset(dst, 0, gw_xadpcm_blocks(samples) * 36);
  reblock_channel(src, src_block, src_channels, channel, samples, dst, 1, 0);
  return gw_xadpcm_blocks(samples) * 36;
}

void gw_ima_decode(const u8* src, u32 src_block, u32 channels, u32 samples, i16* dst) {
  const u32 src_per = (src_block - 4 * channels) * 2 / channels + 1;
  for (u32 c = 0; c < channels; ++c) {
    int pred = 0, idx = 0;
    for (u32 s = 0; s < samples; ++s) {
      const u32 b = s / src_per, k = s % src_per;
      const u8* blk = src + b * src_block;
      if (k == 0) {
        pred = (i16)(blk[c * 4] | blk[c * 4 + 1] << 8);
        idx = blk[c * 4 + 2] > 88 ? 88 : blk[c * 4 + 2];
      } else {
        const u32 code = k - 1;
        const u8* q = blk + 4 * channels + (code / 8) * 4 * channels + 4 * c;
        ima_decode(pred, idx, (q[(code % 8) / 2] >> ((code & 1) * 4)) & 15);
      }
      dst[s * channels + c] = (i16)pred;
    }
  }
}
