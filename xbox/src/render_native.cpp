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

#include <windows.h>
#include <xmmintrin.h>

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
#define GW_DRAW_EVERY (kGwTicks == 30 ? 1 : 2)  // 30 pictures a second either way
#endif
constexpr u32 kDrawEvery = GW_DRAW_EVERY;

// Under load the game keeps its speed and the picture its smoothness gives
// way: the game draws every second frame while that fits in 60 frames a
// second, and every third or fourth (20, 15 pictures a second) when it does
// not. The choice is measured: each frame's time without the pacing wait,
// averaged separately for frames drawn and not, gives what a picture every
// k frames costs a frame on average, (drawn + (k - 1) undrawn) / k; the
// smallest k that fits in a sixtieth of a second (with a margin before going
// back to drawing more often) is used. Without this a crowd put the game
// itself in slow motion. make ADAPTIVE=n keeps kDrawEvery always.
#ifndef GW_DRAW_FIXED
constexpr u32 kDrawMost = kGwTicks == 30 ? 2 : 4;  // 15 pictures a second at the least
#else
constexpr u32 kDrawMost = kDrawEvery;
#endif
u32 g_draw_every = kDrawEvery;

u32 g_frame = 0;   // the game's frames, counted when they render (or not)
u32 g_drawn = 0;   // frames rendered
u32 g_since = 0;   // frames since the last rendered one
bool g_frame_was_drawn;  // this frame rendered (read by the pacing below)

// Whether this frame will be rendered: the update runs before the render in
// the same frame, so the jobs can leave out work only drawing needs.
// (At 30 ticks the jobs' first of two steps a tick builds no vertices:
// src/jobs_native.cpp sets gw_jobs_inner_step meanwhile.)
bool gw_jobs_inner_step;
bool gw_frame_drawn() { return !gw_jobs_inner_step && g_since + 1 >= g_draw_every; }
// The rendered frames' count (this frame's, when it is rendered): the
// particles alternate their buffer sets by it.
u32 gw_drawn_index() { return g_drawn; }

extern "C" void sub_820653C8(PPCContext& ctx, uint8_t* base) {
  const bool draw = gw_frame_drawn();
  ++g_frame;
  g_frame_was_drawn = draw;
  if (!draw) {
    ++g_since;
    return;
  }
  g_since = 0;
  __imp__sub_820653C8(ctx, base);
  ++g_drawn;
}

namespace {
// Frame times (performance-counter ticks, the pacing wait left out) over a
// window, drawn and undrawn frames apart.
i64 g_sum_drawn, g_sum_plain;
u32 g_count_drawn, g_count_plain;

// A frame done: busy its time without the wait. (The log's microseconds
// are per tick at kGwTicks.)
void adapt(i64 busy, i64 period) {
  if (kDrawMost == kDrawEvery) return;
  if (g_frame_was_drawn) {
    g_sum_drawn += busy;
    ++g_count_drawn;
  } else {
    g_sum_plain += busy;
    ++g_count_plain;
  }
  if (g_count_drawn + g_count_plain < 60) return;
  // With a picture every frame there are no undrawn frames to measure (and
  // the reverse): the missing kind is taken as costing as much as the other,
  // which overstates it, so the next choice draws less and measures both.
  i64 drawn = g_count_drawn ? g_sum_drawn / g_count_drawn : 0, plain = g_count_plain ? g_sum_plain / g_count_plain : 0;
  if (!g_count_drawn) drawn = plain;
  if (!g_count_plain) plain = drawn;
  g_sum_drawn = g_sum_plain = 0;
  g_count_drawn = g_count_plain = 0;
  // A frame's average cost at a picture every k frames, against the frame
  // time: within 97% to stay or draw less often, 88% to draw more often.
  auto fits = [&](u32 k, i64 percent) { return (drawn + (i64)(k - 1) * plain) * 100 <= percent * period * (i64)k; };
  u32 want = kDrawMost;
  for (u32 k = kDrawEvery; k <= kDrawMost; ++k) {
    if (fits(k, k < g_draw_every ? 88 : 97)) {
      want = k;
      break;
    }
  }
  if (want != g_draw_every) {
    gw_log("render: a picture every %u frames (frames drawn %u us, not drawn %u us)\n", want,
           (u32)(drawn * 1000000 / (period * kGwTicks)), (u32)(plain * 1000000 / (period * kGwTicks)));
    g_draw_every = want;
  }
}
}  // namespace

// The game's time step (guest 0x8219D280, a float the game's logic reads in
// 98 places): set once by guest 0x8205D538 from the display's refresh, 1 / 60
// (1 / 50 on a 50 Hz PAL display). At 30 ticks a second it is 1 / 30.
#ifdef GW_TICK30
extern "C" void __imp__sub_8205D538(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205D538(PPCContext& ctx, uint8_t* base) {
  __imp__sub_8205D538(ctx, base);
  const float step = 1.0f / 30.0f;
  u32 v;
  memcpy(&v, &step, 4);
  gw_store32(0x8219D280, v);
  gw_log("ticks: 30 a second (time step 1/30)\n");
}
#endif

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

// End of the game's frame and its swap (guest 0x8205E8A8), then the frame
// held to the 60 Hz display: the game steps a fixed 1/60 s per frame, and
// nothing else paces it here (its Direct3D's swap does not wait for the
// display), so a fast frame would make it play fast. Each frame ends no
// earlier than its 1/60 s slot; a frame that runs late does not make the
// next ones hurry (more than two slots behind, the schedule restarts).
extern "C" void __imp__sub_8205E8A8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8205E8A8(PPCContext& ctx, uint8_t* base) {
  __imp__sub_8205E8A8(ctx, base);
  static LARGE_INTEGER frequency;
  static i64 next, frame_start;
  if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
  const i64 period = frequency.QuadPart / kGwTicks;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (frame_start) adapt(now.QuadPart - frame_start, period);
  if (!next || now.QuadPart > next + 2 * period) {
    next = now.QuadPart + period;
    frame_start = now.QuadPart;
    return;
  }
  // Whole milliseconds asleep (Sleep(1) may last up to 2 ms), the rest spun.
  for (;;) {
    QueryPerformanceCounter(&now);
    const i64 left = next - now.QuadPart;
    if (left <= 0) break;
    if (left * 1000 / frequency.QuadPart >= 2) Sleep(1);
    else _mm_pause();
  }
  next += period;
  frame_start = now.QuadPart;
}
