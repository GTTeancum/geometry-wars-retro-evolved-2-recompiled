// PC check of the Xbox ADPCM conversion (src/xadpcm.cpp): every wave of the
// converted banks given, turned into Xbox ADPCM and decoded as the APU's voice
// processor does (each 36-byte block: its header sample, then 63 codes),
// against the converted bank decoded directly. Reports per bank the samples
// that differ and the largest difference.
//   xadpcm_check.exe <bank.adpcm>...
// Built by tools/xadpcm_check.cmd.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <vector>

#include "apu.h"

void gw_log(const char* fmt, ...) { (void)fmt; }

namespace {
const int kSteps[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
                        25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
                        88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
                        307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
                        1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
                        3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
                        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

void step(int& pred, int& idx, int nib) {
  const int st = kSteps[idx];
  int diff = st >> 3;
  if (nib & 1) diff += st >> 2;
  if (nib & 2) diff += st >> 1;
  if (nib & 4) diff += st;
  pred += nib & 8 ? -diff : diff;
  pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
  idx += kIndex[nib];
  idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
}

// Decodes IMA ADPCM blocks of 'block' bytes, 'per' samples a block (the
// header sample and per - 1 codes).
std::vector<short> decode(const unsigned char* d, unsigned block, unsigned ch, unsigned per, unsigned samples) {
  std::vector<short> out(samples * ch);
  for (unsigned c = 0; c < ch; ++c) {
    int pred = 0, idx = 0;
    for (unsigned s = 0; s < samples; ++s) {
      const unsigned b = s / per, k = s % per;
      const unsigned char* blk = d + b * block;
      if (k == 0) {
        pred = (short)(blk[c * 4] | blk[c * 4 + 1] << 8);
        idx = blk[c * 4 + 2] > 88 ? 88 : blk[c * 4 + 2];
      } else {
        const unsigned code = k - 1;
        const unsigned char* q = blk + 4 * ch + (code / 8) * 4 * ch + 4 * c;
        step(pred, idx, (q[(code % 8) / 2] >> ((code & 1) * 4)) & 15);
      }
      out[s * ch + c] = (short)pred;
    }
  }
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  int bad = 0;
  for (int a = 1; a < argc; ++a) {
    FILE* f = fopen(argv[a], "rb");
    if (!f) {
      printf("%s: cannot open\n", argv[a]);
      return 1;
    }
    std::vector<unsigned char> file;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
    fclose(f);
    unsigned count;
    memcpy(&count, &file[4], 4);
    unsigned long long differ = 0, total = 0;
    int worst = 0;
    double noise = 0, signal = 0;
    for (unsigned w = 0; w < count; ++w) {
      unsigned e[8];
      memcpy(e, &file[8 + 32 * w], 32);
      const unsigned ch = e[3], samples = e[4], block = e[5], at = e[6];
      if (!samples || !block) continue;
      const unsigned per = (block - 4 * ch) * 2 / ch + 1;
      // (one entry says more samples than its data holds: only what is there)
      const unsigned have = (e[7] / block) * per + (e[7] % block > 4 * ch ? (e[7] % block - 4 * ch) * 2 / ch + 1 : 0);
      const unsigned s = samples < have ? samples : have;
      std::vector<unsigned char> x(gw_xadpcm_blocks(s) * 36 * ch);
      gw_xadpcm_reblock(&file[at], block, ch, s, x.data());
      const std::vector<short> ref = decode(&file[at], block, ch, per, s);
      const std::vector<short> got = decode(x.data(), 36 * ch, ch, 64, s);
      for (size_t i = 0; i < ref.size(); ++i) {
        const int d = abs(ref[i] - got[i]);
        noise += (double)d * d;
        signal += (double)ref[i] * ref[i];
        if (d) ++differ;
        if (d > worst) {
          worst = d;
          if (getenv("XADPCM_WORST")) {
            const size_t i0 = i >= 6 * ch ? i - 6 * ch : 0;
            printf("  worst so far: wave %u sample %zu (channel %zu, source block %zu, at %zu in it):", w, i / ch,
                   i % ch, i / ch / per, i / ch % per);
            for (size_t j = i0; j < i + 6 * ch && j < ref.size(); j += ch) printf(" %d/%d", ref[j], got[j]);
            printf("\n");
          }
        }
      }
      total += ref.size();
    }
    printf("%s: %u waves, %llu of %llu samples differ (%.4f%%), largest difference %d, added noise %.1f dB "
           "below the sound\n",
           argv[a], count, differ, total, total ? 100.0 * differ / total : 0.0, worst,
           noise > 0 ? 10.0 * log10(signal / noise) : 999.0);
    if (worst > 4096) bad = 1;
  }
  return bad;
}
