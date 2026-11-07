// The particle system's output buffers, shrunk to fit 64 MB.
//
// At start-up the game (guest 0x820673C8) makes two tables of 18 buffers,
// one entry per frame in flight (0..2, at 0x821B0000 + 16004) and particle job
// (0..5), picked as frame * 6 + job by the particle job (guest 0x82063A38):
//   4 MB buffers (0x821B0000 + 16000): 128 bytes per live particle, the
//     quads of the main picture (vertex shader 6FA9F218, 32-byte vertices);
//   3.5 MB buffers (+ 15996): 112 bytes per particle, the glow pass's copy
//     (vertex shader C5C713E2, 28-byte vertices).
// Committed memory follows the busiest frame each buffer has seen, so over a
// level the 36 buffers grew past what 64 MB holds.
//
// Here:
//   - the main-picture buffers keep two frames in flight instead of three:
//     the third frame shares the first frame's set (the draws read a frame's
//     particles while the next frame fills the other set);
//   - the glow-pass buffers likewise (their quads are the light splats of
//     the grid's colour field, src/nv2a.cpp);
//   - each buffer's first part has a host mirror (src/vertex_mirror.cpp)
//     that src/particle_native.cpp fills in the NV2A's order and the
//     renderer draws in place: 64 KB of a main buffer (512 particles a job),
//     56 KB of a glow buffer, past which the guest buffer is used.
#include "runtime.h"

extern "C" void sub_820E8130(PPCContext& ctx, uint8_t* base);  // game heap allocation
extern "C" void sub_8206A480(PPCContext& ctx, uint8_t* base);  // physical buffer allocation
extern "C" void sub_82063EC0(PPCContext& ctx, uint8_t* base);  // particle system set-up (rest)

namespace {

constexpr u32 kTableMain = 0x821B0000 + 16000, kTableGlow = 0x821B0000 + 15996;
constexpr u32 kJobs = 6, kFramesInFlight = 3, kEntries = kJobs * kFramesInFlight;
constexpr u32 kMainFrames = 2;
constexpr u32 kMainSize = 0x400000, kGlowSize = 0x380000;
constexpr u32 kMainMirror = 512 * 128, kGlowMirror = 512 * 112;

// The two sets' buffers by set and job (gw_particle_set_buffer).
u32 g_set_main[kMainFrames][kJobs], g_set_glow[kMainFrames][kJobs];

u32 call_alloc(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), u32 size) {
  ctx.r3.u64 = size;
  fn(ctx, base);
  return ctx.r3.u32;
}

}  // namespace

// A set's buffer for a job (main picture or glow pass). The particle job
// (src/particle_native.cpp) picks the set by drawn frame, not by the game's
// frame in flight: three frames in flight on two sets, with every second
// frame drawn, made one drawn frame in three write the set the frame drawn
// before it used, while the GPU could still be reading it (whole quads
// half-written: large stray polygons for a frame, most with many particles).
u32 gw_particle_set_buffer(u32 set, u32 job, bool glow) {
  return (glow ? g_set_glow : g_set_main)[set % kMainFrames][job % kJobs];
}

// Particle system initialisation (guest 0x820673C8).
extern "C" void sub_820673C8(PPCContext& ctx, uint8_t* base) {
  const u32 main = call_alloc(ctx, base, sub_820E8130, kEntries * 4);
  gw_store32(kTableMain, main);
  const u32 glow = call_alloc(ctx, base, sub_820E8130, kEntries * 4);
  gw_store32(kTableGlow, glow);
  for (u32 i = 0; i < kEntries; ++i) {
    const u32 own = i % (kMainFrames * kJobs);
    if (own == i) {
      const u32 m = call_alloc(ctx, base, sub_8206A480, kMainSize);
      const u32 g = call_alloc(ctx, base, sub_8206A480, kGlowSize);
      gw_vertex_mirror_add(m, kMainMirror);
      gw_vertex_mirror_add(g, kGlowMirror);
      g_set_main[i / kJobs][i % kJobs] = m;
      g_set_glow[i / kJobs][i % kJobs] = g;
      gw_store32(main + i * 4, m);
      gw_store32(glow + i * 4, g);
    } else {
      gw_store32(main + i * 4, gw_load32(main + own * 4));
      gw_store32(glow + i * 4, gw_load32(glow + own * 4));
    }
  }
  gw_log("particle buffers: main picture and glow pass %u sets for %u frames in flight, mirrored\n", kMainFrames,
         kFramesInFlight);
  sub_82063EC0(ctx, base);
}
