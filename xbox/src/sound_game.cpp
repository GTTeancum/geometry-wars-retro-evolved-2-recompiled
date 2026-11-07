// The game's sound layer, native: the five functions through which the
// game's code reaches its sound (Bizarre's audio engine over XAudio2, which
// does not run on the Xbox) go to the native engine (src/sound_native.cpp)
// instead. The game passes only events (0..112: "Fire_normal",
// "Enemy_explode"...) and numbered float parameters (0..21: Thrust, Paused,
// SFX_Volume, Music_Volume, Score...); what each sounds like is in the sound
// graphs (audio\*.dat), which the native engine plays.
//
// Because the game's engine is never created (its manager pointer at
// 0x821D3404 stays 0), the rest of its sound layer finds no manager and does
// nothing: the reverb set-up and read-back (0x82078C88, 0x82079318) check
// for it. None of XAudio2, its threads or the XMA decoder start.
#include "runtime.h"

extern "C" void sub_82034588(PPCContext& ctx, uint8_t* base);  // the game's paused state

namespace {
constexpr u32 kGravityWells = 0x821D1E38;  // 5 x {owner, value, active byte}
constexpr u32 kPausedFlag = 0x821D1E74;    // the paused state last passed on
}  // namespace

// Audio initialisation (guest 0x8207A048): the native engine instead of the
// game's (sub_820799D8); the gravity well table cleared and the events
// Silence (38) and Game_start (0) fired, as the original.
extern "C" void sub_8207A048(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  gw_sound_start();
  for (u32 i = 0; i < 5; ++i) {
    gw_store32(kGravityWells + i * 12, 0);
    gw_store32(kGravityWells + i * 12 + 4, 0);
    gw_store8(kGravityWells + i * 12 + 8, 0);
  }
  gw_sound_event(38);
  gw_sound_event(0);
}

// Fire an event (guest 0x820787F8): r3 the event.
extern "C" void sub_820787F8(PPCContext& ctx, uint8_t* base) {
  (void)base;
  gw_sound_event(ctx.r3.u32);
}

// Set a parameter (guest 0x820783A8): r3 the parameter, f1 its value.
extern "C" void sub_820783A8(PPCContext& ctx, uint8_t* base) {
  (void)base;
  gw_sound_param(ctx.r3.u32, (float)ctx.f1.f64);
}

// Song 3's TransitionSwitch output (guest 0x82078520) -> f1 (the game waits
// for it to pass 0.5 before moving on in that mode).
extern "C" void sub_82078520(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.f1.f64 = gw_sound_output(0);
}

// Pause (guest 0x820782E0): when the game's paused state (sub_82034588)
// changes, it is kept at kPausedFlag and passed on (the original paused the
// six songs' graphs).
extern "C" void sub_820782E0(PPCContext& ctx, uint8_t* base) {
  ctx.r1.u64 = ctx.r1.u32 - 128;
  sub_82034588(ctx, base);
  ctx.r1.u64 = ctx.r1.u32 + 128;
  const u8 paused = (u8)ctx.r3.u32;
  if (paused == gw_load8(kPausedFlag)) return;
  gw_store8(kPausedFlag, paused);
  gw_sound_pause(paused != 0);
}

// The per-frame sound update (guest 0x82079BC0): the game's state as the
// sound graphs' parameters, as the original computes them; the original
// ended with a call into the game's (never created) audio manager.
extern "C" void sub_82033610(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82033600(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820336B8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82093600(PPCContext& ctx, uint8_t* base);  // the options
extern "C" void sub_820392F0(PPCContext& ctx, uint8_t* base);  // the progression level
namespace {
float load_float(u32 a) {
  const u32 v = gw_load32(a);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
u32 call(void (*fn)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base) {
  ctx.r1.u64 = ctx.r1.u32 - 160;  // the original's frame
  fn(ctx, base);
  ctx.r1.u64 = ctx.r1.u32 + 160;
  return ctx.r3.u32;
}
i32 truncate(float f) { return f != f ? (i32)0x80000000 : f >= 2147483647.0f ? 0x7FFFFFFF : f <= -2147483648.0f ? (i32)0x80000000 : (i32)f; }
// A list of game objects: head node at list + 4, node {object +0, next +4};
// an object with bit 0 of +36 set is gone.
template <typename F>
void each_object(u32 list, F f) {
  for (u32 node = gw_load32(list + 4); node; node = gw_load32(node + 4)) {
    const u32 object = gw_load32(node);
    if (!object) break;
    if (!(gw_load32(object + 36) & 1)) f(object);
  }
}
}  // namespace

extern "C" void sub_82079BC0(PPCContext& ctx, uint8_t* base) {
  constexpr u32 C = 0x82000000;
  const float one = load_float(C + 1732), zero = load_float(C + 1740);
  if ((u8)call(sub_82033610, ctx, base)) {
    call(sub_82033600, ctx, base);
  } else {
    call(sub_820336B8, ctx, base);
  }
  const u32 options = call(sub_82093600, ctx, base);
  sub_820782E0(ctx, base);
  const bool paused = (u8)call(sub_82034588, ctx, base) != 0;
  gw_sound_param(1, paused ? one : zero);                              // Paused
  gw_sound_param(8, load_float(options + 4) * load_float(options + 4));  // SFX_Volume
  gw_sound_param(2, (float)(i32)gw_load32(0x8219F2FC));               // Mayfly_numbers

  // DistanceFromHill: the nearest player to any hill zone (list 0x82181438)
  // beyond its radius (+32), from players (list 0x821813A8) at +4, +8.
  constexpr u32 kPlayers = 0x821813A8, kHills = 0x82181438;
  float nearest = load_float(C + 1936);
  each_object(kHills, [&](u32 hill) {
    each_object(kPlayers, [&](u32 player) {
      const float dx = load_float(hill + 4) - load_float(player + 4);
      const float dy = load_float(hill + 8) - load_float(player + 8);
      float d = __builtin_sqrtf(dy * dy + dx * dx) - load_float(hill + 32);
      d = d >= 0.0f ? d : zero;
      nearest = d - nearest >= 0.0f ? nearest : d;
    });
  });
  gw_sound_param(14, nearest);

  // Music_Volume: the option squared while music plays (0x82181460) and not
  // muted (byte 0x821D3798), else 0.
  const bool music = gw_load32(0x82181460) != 0 && gw_load8(0x821D3798) == 0;
  gw_sound_param(9, music ? load_float(options + 8) * load_float(options + 8) : zero);

  // Score and Multiplier: the highest of the players' (records of 120 bytes
  // from 0x82199470 + 15392: score 64-bit at +0, multiplier at +24).
  constexpr u32 kScores = 0x82199470;
  i64 score = 0;
  i32 multiplier = 0;
  const i32 records = (i32)gw_load32(kScores + 15348);
  for (i32 i = 0; i < records; ++i) {
    const u32 at = kScores + 15416 + (u32)i * 120;
    const i64 s = (i64)gw_load64(at - 24);
    if (s > score) score = s;
    const i32 m = (i32)gw_load32(at);
    if (m > multiplier) multiplier = m;
  }

  // Thrust: the players' average (+176); MagnetActive: one of them has +220 > 0.
  float thrust = zero, magnet = zero;
  i32 players = 0;
  each_object(kPlayers, [&](u32 player) {
    if (load_float(player + 220) > zero) magnet = one;
    thrust = load_float(player + 176) + thrust;
    ++players;
  });
  if (players > 0) thrust = thrust / (float)players;
  // PlayerPosX / Y: the first player's.
  float x = zero, y = zero;
  bool found = false;
  each_object(kPlayers, [&](u32 player) {
    if (found) return;
    found = true;
    x = load_float(player + 4);
    y = load_float(player + 8);
  });
  gw_sound_param(0, thrust);
  gw_sound_param(17, x);
  gw_sound_param(18, y);
  gw_sound_param(15, (float)(i32)call(sub_820392F0, ctx, base));  // ProgressionLevelNumber
  gw_sound_param(10, (float)multiplier);
  gw_sound_param(11, (float)score);
  gw_sound_param(12, (float)(i32)gw_load32(0x82198828));  // NoEnemies
  // Time and LevelTime: whole seconds of two clocks, clamped to 0..total.
  auto clock = [&](u32 total_at, u32 used_at) {
    const i32 total = truncate(load_float(kScores + total_at)), used = truncate(load_float(kScores + used_at));
    const i32 left = total - used;
    return (float)(left < 0 ? 0 : left > total ? total : left);
  };
  gw_sound_param(13, clock(15848, 14840));
  gw_sound_param(19, clock(15852, 14844));
  gw_sound_param(20, (float)(i32)gw_load32(0x82197284));  // BombKillCount
  gw_sound_param(21, magnet);
}

// The debug song unload / reload (no callers in the game): nothing to do.
extern "C" void sub_82078018(PPCContext&, uint8_t*) {}
extern "C" void sub_82078360(PPCContext&, uint8_t*) {}
extern "C" void sub_820786E0(PPCContext&, uint8_t*) {}
