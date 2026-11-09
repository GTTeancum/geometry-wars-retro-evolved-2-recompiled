// Turns converted sound banks (IMA ADPCM in Microsoft blocks, 'GWAD') into
// Xbox ADPCM banks ('GWX1'), the format the APU's voice processor plays and
// the native engine decodes (src/sound_native.cpp):
//   'GWX1', wave count, then per wave the same eight little-endian words as
//   'GWAD' (original offset and size, rate, channels, samples, group size,
//   data offset, data size), then the data: per wave, Xbox ADPCM blocks (36
//   bytes per channel: a header sample and step index, then 63 played codes
//   and an unplayed one; 64 samples), interleaved per block for stereo, in
//   groups of 28 mono or 14 stereo blocks (1008 bytes, one stream read slot).
// The conversion is src/xadpcm.cpp's (installer/SoundConvert.cs does the same).
//   xadpcm_bank.exe <in.adpcm> <out.adpcm>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "apu.h"

void gw_log(const char* fmt, ...) { (void)fmt; }

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: xadpcm_bank <in.adpcm> <out.adpcm>\n");
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "%s: cannot open\n", argv[1]);
    return 1;
  }
  std::vector<unsigned char> in;
  unsigned char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) in.insert(in.end(), buf, buf + n);
  fclose(f);
  unsigned head[2];
  memcpy(head, in.data(), 8);
  if (head[0] != 0x44415747) {
    fprintf(stderr, "%s: not a GWAD bank\n", argv[1]);
    return 1;
  }
  const unsigned count = head[1], table = 8 + 32 * count;
  std::vector<unsigned> entries(8 * count);
  memcpy(entries.data(), &in[8], 32 * count);
  std::vector<unsigned char> data;
  for (unsigned w = 0; w < count; ++w) {
    unsigned* e = &entries[8 * w];
    const unsigned ch = e[3], block = e[5], at = e[6], size = e[7];
    unsigned samples = e[4];
    if (ch < 1 || ch > 2 || block <= 4 * ch || !samples) {
      e[4] = e[5] = e[6] = e[7] = 0;
      continue;
    }
    // (the samples its data holds: one entry says more than it has)
    const unsigned per = (block - 4 * ch) * 2 / ch + 1;
    unsigned have = (size / block) * per;
    if (size % block > 4 * ch) have += (size % block - 4 * ch) * 2 / ch + 1;
    if (samples > have) samples = have;
    std::vector<unsigned char> x(gw_xadpcm_blocks(samples) * 36 * ch);
    gw_xadpcm_reblock(&in[at], block, ch, samples, x.data());
    e[4] = samples;
    e[5] = (ch == 1 ? 28 : 14) * 36 * ch;
    e[6] = table + (unsigned)data.size();
    e[7] = (unsigned)x.size();
    data.insert(data.end(), x.begin(), x.end());
    while (data.size() % 4) data.push_back(0);
  }
  FILE* o = fopen(argv[2], "wb");
  if (!o) {
    fprintf(stderr, "%s: cannot write\n", argv[2]);
    return 1;
  }
  const unsigned out_head[2] = {0x31585747, count};  // 'GWX1'
  fwrite(out_head, 4, 2, o);
  fwrite(entries.data(), 4, entries.size(), o);
  fwrite(data.data(), 1, data.size(), o);
  fclose(o);
  printf("%s: %u waves, %u KB\n", argv[2], count, (unsigned)((table + data.size()) / 1024));
  return 0;
}
