// PC check of the Xenos -> NV2A vertex program translator (src/xenos_vp.cpp).
// For each captured vertex shader (gpu_capture.py --shaders):
//   1. translate it (textured: uv 0 / colour 1, and untextured: colour 0);
//   2. run the NV2A program in a small simulator of the NV2A vertex unit and
//      the original shader in the CPU interpreter (src/xenos_vs.cpp), on the
//      same random vertices and constants, and compare position, colour and
//      texture coordinates.
//   vp_check.exe xbox/test-artifacts/gpu/hash_vs_*.bin
// Built 32-bit (xbox/tools/vp_check.cmd) so guest addresses wrap as on the
// Xbox: guest 0x8E000000 is host 0x30000000, guest 0xC0000000 (GPU address
// 0) is host 0x62000000.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include <initializer_list>

#include "xenos_vs.h"

void gw_log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

void gw_fatal(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  exit(1);
}

namespace {

// --- NV2A vertex unit simulator (encoding as in pbkit nv20_shader.h) -----------

struct Vec {
  float v[4];
};

Vec read_source(u32 s, int const_index, int input, const Vec* r, const Vec* in, const Vec* c) {
  Vec base{};
  switch (s & 3) {
    case 1: base = r[(s >> 2) & 0xF]; break;
    case 2: base = in[input]; break;
    case 3: base = c[const_index]; break;
  }
  Vec out;
  const int shifts[4] = {12, 10, 8, 6};
  for (int i = 0; i < 4; ++i) out.v[i] = base.v[(s >> shifts[i]) & 3];
  if (s & (1u << 14)) {
    for (float& f : out.v) f = -f;
  }
  return out;
}

void write_masked(Vec& dst, const Vec& value, u32 nv_mask) {
  for (int i = 0; i < 4; ++i) {
    if (nv_mask & (8u >> i)) dst.v[i] = value.v[i];
  }
}

// Runs a program; outputs[0] = position, [3] = diffuse, [9] = texcoord 0.
void run_nv2a(const u32* words, u32 count, const Vec* in, const Vec* c, Vec* outputs) {
  Vec r[16] = {};
  for (u32 n = 0; n < count; ++n) {
    const u32* w = &words[n * 4];
    u32 ilu = (w[1] >> 25) & 0xF, mac = (w[1] >> 21) & 0xF;
    int ci = (int)((w[1] >> 13) & 0xFF) - 96, input = (int)((w[1] >> 9) & 0xF);
    u32 a = ((w[1] & 0x1FF) << 6) | (w[2] >> 26);
    u32 b = (w[2] >> 11) & 0x7FFF;
    u32 cc = ((w[2] & 0x7FF) << 4) | (w[3] >> 28);
    u32 mac_mask = (w[3] >> 24) & 0xF, temp = (w[3] >> 20) & 0xF, ilu_mask = (w[3] >> 16) & 0xF;
    u32 out_mask = (w[3] >> 12) & 0xF, orb = (w[3] >> 11) & 1, addr = (w[3] >> 3) & 0xFF, mux = (w[3] >> 2) & 1;
    if (ci < 0 || ci >= 192) ci = 0;
    Vec A = read_source(a, ci, input, r, in, c), B = read_source(b, ci, input, r, in, c),
        C = read_source(cc, ci, input, r, in, c);
    Vec m{}, l{};
    switch (mac) {
      case 1: m = A; break;
      case 2: for (int i = 0; i < 4; ++i) m.v[i] = A.v[i] * B.v[i]; break;
      case 3: for (int i = 0; i < 4; ++i) m.v[i] = A.v[i] + C.v[i]; break;
      case 4: for (int i = 0; i < 4; ++i) m.v[i] = A.v[i] * B.v[i] + C.v[i]; break;
      case 5: { float d = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2]; m = {{d, d, d, d}}; break; }
      case 7: { float d = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2] + A.v[3] * B.v[3]; m = {{d, d, d, d}}; break; }
      case 9: for (int i = 0; i < 4; ++i) m.v[i] = A.v[i] < B.v[i] ? A.v[i] : B.v[i]; break;
      case 10: for (int i = 0; i < 4; ++i) m.v[i] = A.v[i] >= B.v[i] ? A.v[i] : B.v[i]; break;
      case 11: for (int i = 0; i < 4; ++i) m.v[i] = A.v[i] < B.v[i] ? 1.0f : 0.0f; break;
      case 12: for (int i = 0; i < 4; ++i) m.v[i] = A.v[i] >= B.v[i] ? 1.0f : 0.0f; break;
      default: break;
    }
    float x = C.v[0];
    switch (ilu) {
      case 1: l = C; break;
      case 2: l.v[0] = l.v[1] = l.v[2] = l.v[3] = 1.0f / x; break;
      case 3: { float v = 1.0f / x; l.v[0] = l.v[1] = l.v[2] = l.v[3] = v; break; }
      case 4: l.v[0] = l.v[1] = l.v[2] = l.v[3] = 1.0f / sqrtf(fabsf(x)); break;
      case 5: l = {{exp2f(floorf(x)), x - floorf(x), exp2f(x), 1.0f}}; break;
      case 6: { float lg = log2f(fabsf(x)); l = {{floorf(lg), fabsf(x) / exp2f(floorf(lg)), lg, 1.0f}}; break; }
      default: break;
    }
    if (mac) write_masked(r[temp], m, mac_mask);
    if (ilu) write_masked(r[mac ? 1 : temp], l, ilu_mask);
    if (out_mask && orb) write_masked(outputs[addr & 0xF], mux ? l : m, out_mask);
    if (w[3] & 1) break;
  }
}

// --- test harness -----------------------------------------------------------------

constexpr u32 kShaderGuest = 0x8E000000;
constexpr u32 kDataGuest = 0xC0000000;  // GPU address 0
u32 g_regs[0x6000];

float random_float(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }

u32 bits_of(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  return u;
}

float float_of(u32 u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}

bool close(float a, float b, float tolerance) { return fabsf(a - b) <= tolerance * (1.0f + fabsf(b)); }

// Compares the two paths for one pairing; returns the number of mismatches.
int compare(const XvsProgram* program, const XvpProgram* gp, int uv_i, int color_i, int vertices) {
  int bad = 0;
  for (int vtx = 0; vtx < vertices; ++vtx) {
    // Random vertex data (big-endian floats in guest memory) and constants.
    for (u32 k = 0; k < gp->stride / 4; ++k) gw_store32(kDataGuest + vtx * gp->stride + k * 4, bits_of(random_float(-2, 2)));
    float cpu_pos[4], interp[8][4];
    xvs_run(program, g_regs, (u32)vtx, cpu_pos, interp);
    if (fabsf(cpu_pos[3]) < 0.25f) continue;  // near the divide's pole

    Vec in[16] = {};
    for (u32 a = 0; a < gp->attr_count; ++a) {
      const auto& at = gp->attrs[a];
      in[a].v[3] = 1;
      for (u32 k = 0; k < at.components; ++k) in[a].v[k] = float_of(gw_load32(kDataGuest + vtx * gp->stride + at.offset + k * 4));
    }
    Vec c[192] = {};
    static const float kFixed[12] = {0, 1, 2, 3, 0.15915494f, 0.5f, 6.2831853f, 3.1415927f,
                                     1.2732395f, -0.40528473f, 0.225f, 1.5707963f};
    memcpy(c, kFixed, sizeof(kFixed));
    c[3] = {{1, 1, 0, 0}};  // viewport: identity
    for (u32 i = 0; i < gp->const_count; ++i) {
      for (int k = 0; k < 4; ++k) c[4 + i].v[k] = float_of(g_regs[0x4000 + gp->const_xenos[i] * 4 + k]);
    }
    Vec out[16] = {};
    run_nv2a(gp->words, gp->instructions, in, c, out);

    float want_x = cpu_pos[0] / cpu_pos[3], want_y = cpu_pos[1] / cpu_pos[3];
    bool ok = close(out[0].v[0], want_x, 0.01f) && close(out[0].v[1], want_y, 0.01f);
    for (int k = 0; k < 4 && color_i >= 0; ++k) ok &= close(out[3].v[k], interp[color_i][k], 0.01f);
    for (int k = 0; k < 2 && uv_i >= 0; ++k) {
      ok &= close(out[9].v[k], interp[uv_i][k] * (k ? 3.0f : 2.0f), 0.01f);  // texture scale (c0.z, c0.w)
      for (int unit = 10; unit <= 12; ++unit) ok &= out[unit].v[k] == out[9].v[k];
    }
    if (!ok && bad++ < 3) {
      printf("    vertex %d: pos %.4f %.4f want %.4f %.4f; colour %.3f %.3f %.3f %.3f want %.3f %.3f %.3f %.3f",
             vtx, out[0].v[0], out[0].v[1], want_x, want_y, out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3],
             color_i >= 0 ? interp[color_i][0] : 0, color_i >= 0 ? interp[color_i][1] : 0,
             color_i >= 0 ? interp[color_i][2] : 0, color_i >= 0 ? interp[color_i][3] : 0);
      if (uv_i >= 0) printf("; uv %.3f %.3f want %.3f %.3f", out[9].v[0], out[9].v[1], interp[uv_i][0], interp[uv_i][1]);
      printf("\n");
    }
  }
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  for (u32 guest : {kShaderGuest, kDataGuest}) {
    u8* host = gw_host(guest);
    if (VirtualAlloc(host, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != host) {
      printf("could not map guest %08X at host %p\n", guest, host);
      return 1;
    }
  }
  srand(1);
  int translated = 0, matched = 0;
  for (int i = 1; i < argc; ++i) {
    FILE* f = fopen(argv[i], "rb");
    if (!f) continue;
    u32 bytes = (u32)fread(gw_host(kShaderGuest), 1, 0x8000, f);
    fclose(f);
    u32 dwords = bytes / 4;
    if (strstr(argv[i], "_ps_")) {
      // Pixel shader: print it as a weighted sum of texture samples.
      static u32 words[1024];
      for (u32 k = 0; k < dwords && k < 1024; ++k) words[k] = gw_load32(kShaderGuest + k * 4);
      const XpsSum sum = xps_sum_of(words, dwords);
      printf("%s: %d terms:", argv[i], sum.count);
      for (int t = 0; t < sum.count; ++t) {
        const XpsSum::Term& term = sum.terms[t];
        printf(" %s%ux tf%u", t ? "+ " : "", term.repeat + 1, term.texture);
        for (int k = 0; k < term.weight_count; ++k) printf(" * c%u.%c", term.weight[k] / 4, "xyzw"[term.weight[k] % 4]);
      }
      printf("\n");
      continue;
    }
    u32 hash = 0x811C9DC5;
    for (u32 k = 0; k < dwords; ++k) hash = (hash ^ gw_load32(kShaderGuest + k * 4)) * 0x01000193;
    printf("%s: hash %08X, %u dwords\n", argv[i], hash, dwords);
    const XvsProgram* program = xvs_load(hash, kShaderGuest, dwords);
    if (!program) {
      printf("  decode failed\n");
      continue;
    }
    const XvpProgram* a = xvp_get(program, 0, 1, true);
    const XvpProgram* b = xvp_get(program, -1, 0, true);
    if (!a || !b) continue;
    ++translated;
    // Random constants, stream at GPU address 0 (fetch constant type 3,
    // endian 8-in-32).
    for (u32 k = 0; k < 256 * 4; ++k) g_regs[0x4000 + k] = bits_of(random_float(-2, 2));
    g_regs[0x4800 + a->fetch_slot * 2] = 3;
    g_regs[0x4800 + a->fetch_slot * 2 + 1] = 2;
    int bad = compare(program, a, 0, 1, 200) + compare(program, b, -1, 0, 200);
    printf("  %s\n", bad ? "MISMATCH" : "matches the interpreter");
    if (!bad) ++matched;
  }
  printf("%d of %d shaders translate, %d match the interpreter\n", translated, argc - 1, matched);
  return 0;
}
