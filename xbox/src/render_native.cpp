// The game's frame rendering (guest 0x820653C8), cut down to what the Xbox
// renderer draws.
//
// Each frame the game renders, through its Direct3D (from a level capture):
//   1. the grid's colour field (sub_82065930): light splats into a 150x100
//      target, then a shrink/blur pyramid of it (sub_82064930 per level) and
//      a combine (sub_82064C90);
//   2. the scene (sub_820663A8, sub_820669F0): stars, grid lines, ships,
//      particles, the HUD;
//   3. the glow: two more shrink/blur pyramids of the scene (sub_82064930
//      per level, five full-screen passes and five resolves each) and their
//      sums (sub_82064F88);
//   4. the composite of scene and glow to the front buffer.
// The Xbox renderer (src/nv2a.cpp) draws the splats, the scene and its own
// glow and colour field, and skips every pass of the pyramids, sums and
// composite (render targets it does not reproduce). Building those passes
// was most of the game's Direct3D time on the main thread: shader, texture
// and constant state for about 90 full-screen passes and their resolves a
// frame, then the command processor reading all of it to throw it away.
//
// So the pyramid level, the pyramid sum and the field combine do nothing
// here. They only set Direct3D state and queue GPU work whose results only
// those passes read; every later pass sets its own render target, shaders
// and textures first.
#include "runtime.h"

extern "C" void __imp__sub_820653C8(PPCContext& ctx, uint8_t* base);

// The game's frame (guest 0x82037E00) is: update the game, begin the frame
// (sub_8205E670), render it (sub_820653C8), end it and swap (sub_8205E8A8).
// The game steps a fixed 1/60 s per frame, and the Xbox cannot both update
// and draw 60 times a second (nor show 720p at 60): the game updates every
// frame and renders every kDrawEvery-th one, so it runs at full speed and
// shows 60 / kDrawEvery frames a second. The begin and the swap still run
// every frame; a swap with nothing drawn keeps the frame on screen
// (gw_nv2a_swap), and pacing the frames to the display is unchanged.
#ifndef GW_DRAW_EVERY
#define GW_DRAW_EVERY 2
#endif
constexpr u32 kDrawEvery = GW_DRAW_EVERY;

u32 g_frame = 0;  // the game's frames, counted when they render (or not)

// Whether this frame will be rendered: the update runs before the render in
// the same frame, so the jobs can leave out work only drawing needs.
bool gw_frame_drawn() { return g_frame % kDrawEvery == 0; }

extern "C" void sub_820653C8(PPCContext& ctx, uint8_t* base) {
  const bool draw = gw_frame_drawn();
  ++g_frame;
  if (!draw) return;
  __imp__sub_820653C8(ctx, base);
}

// A pyramid level: shrink, blur across, blur down, two more blurs, each with a
// resolve. r3 the level's textures and targets.
extern "C" void sub_82064930(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
}

// A pyramid's sum of levels, resolved.
extern "C" void sub_82064F88(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
}

// The colour field's combine of its levels with last frame's field.
extern "C" void sub_82064C90(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
}
