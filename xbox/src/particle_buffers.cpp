// The particle system's output buffers, shrunk to fit 64 MB.
//
// At start-up the game (guest 0x820673C8) makes two tables of 18 buffers:
// 4 MB ones (0x821B0000 + 16000) and 3.5 MB ones (+ 15996). The particle job
// (guest 0x82063A38) picks entry frame * 6 + job, for the frame in flight
// (0..2, at + 16004) and its job (0..5), and writes 128 + 112 bytes per live
// particle. Committed memory follows the busiest frame each buffer has seen,
// so over a level the 36 buffers grew past what 64 MB holds.
//
// Here the third frame shares the first frame's buffers: two frames in flight
// instead of three. The draws read a frame's particles while the next frame
// fills the other set.
#include "runtime.h"

extern "C" void sub_820E8130(PPCContext& ctx, uint8_t* base);  // game heap allocation
extern "C" void sub_8206A480(PPCContext& ctx, uint8_t* base);  // physical buffer allocation
extern "C" void sub_82063EC0(PPCContext& ctx, uint8_t* base);  // particle system set-up (rest)

namespace {

constexpr u32 kTableLarge = 0x821B0000 + 16000, kTableSmall = 0x821B0000 + 15996;
constexpr u32 kJobs = 6, kFramesInFlight = 3, kEntries = kJobs * kFramesInFlight;
constexpr u32 kRealFrames = 2;

u32 call_alloc(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), u32 size) {
  ctx.r3.u64 = size;
  fn(ctx, base);
  return ctx.r3.u32;
}

}  // namespace

// Particle system initialisation (guest 0x820673C8).
extern "C" void sub_820673C8(PPCContext& ctx, uint8_t* base) {
  const u32 large = call_alloc(ctx, base, sub_820E8130, kEntries * 4);
  gw_store32(kTableLarge, large);
  const u32 small = call_alloc(ctx, base, sub_820E8130, kEntries * 4);
  gw_store32(kTableSmall, small);
  for (u32 i = 0; i < kEntries; ++i) {
    const bool shared = i >= kRealFrames * kJobs;
    const u32 from = i % (kRealFrames * kJobs);
    gw_store32(large + i * 4, shared ? gw_load32(large + from * 4) : call_alloc(ctx, base, sub_8206A480, 0x400000));
  }
  for (u32 i = 0; i < kEntries; ++i) {
    const bool shared = i >= kRealFrames * kJobs;
    const u32 from = i % (kRealFrames * kJobs);
    gw_store32(small + i * 4, shared ? gw_load32(small + from * 4) : call_alloc(ctx, base, sub_8206A480, 0x380000));
  }
  gw_log("particle buffers: %u frames in flight share %u sets\n", kFramesInFlight, kRealFrames);
  sub_82063EC0(ctx, base);
}
