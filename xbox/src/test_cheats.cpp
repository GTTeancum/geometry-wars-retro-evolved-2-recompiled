// Test only (make TEST_CHEATS=y): what it takes to reach a long game's load
// with the remote pad (tools/xemu_pad.py), as the PC port's cheats do
// (docs/cheats.md, src/cheat_hooks.cpp, src/unlock_all_modes.cpp):
// - Lives never run out (or invincible, or neither: gw_cheat_invincible below):
//   Player::OnHit (sub_8204EEA8); invincible returns at once for a player
//   (an object whose first word is the Player vtable, 0x82001EB8).
// - Every mode unlocked in memory: the profile record's mode mask (0x821F47B8
//   + 16, the local player's 48-byte record) reads all six modes while the
//   game runs; the real mask is put back for each profile write and taken
//   afresh at each profile load, so the save never changes. (A mode unlocked
//   during a test game is not kept.)
#ifdef GW_TEST_CHEATS
#include "runtime.h"

namespace {
constexpr u32 kPlayerVtable = 0x82001EB8;
constexpr u32 kMaskAddress = 0x821F47B8 + 16;
constexpr u32 kAllModes = 0x3F;
u32 g_real_mask;
bool g_have_real;

void force_all() {
  if (!g_have_real) return;
  gw_store32(kMaskAddress, kAllModes);
}
}  // namespace

// What a hit does to the player, switchable while the game runs
// (tools/xbdm_poke.py gw_cheat_invincible N):
//   0  as the game: a life lost, game over at none;
//   1  invincible (the hit ignored): the enemies that reach the ship are
//      never cleared and pile up on it for as long as the game lasts, which
//      no real game reaches;
//   2  (default) lives never run out: the ship dies, the screen clears and it
//      comes back, as in a real game, with the lives (guest 0x8219D098)
//      topped up to 4 before each hit.
extern "C" {
volatile u32 gw_cheat_invincible = 2;
}
constexpr u32 kLives = 0x8219D098;

extern "C" void __imp__sub_8204EEA8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8204EEA8(PPCContext& ctx, uint8_t* base) {
  const u32 object = ctx.r3.u32;
  const bool player = object && gw_load32(object) == kPlayerVtable;
  if (player && gw_cheat_invincible == 1) return;
  if (player && gw_cheat_invincible == 2 && (int)gw_load32(kLives) < 4) gw_store32(kLives, 4);
  __imp__sub_8204EEA8(ctx, base);
}

// The profile parsed into the live record.
extern "C" void __imp__sub_8207F5B8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8207F5B8(PPCContext& ctx, uint8_t* base) {
  __imp__sub_8207F5B8(ctx, base);
  g_real_mask = gw_load32(kMaskAddress);
  g_have_real = true;
  gw_log("test cheats: real mode mask %02X, all modes shown\n", g_real_mask);
  force_all();
}

// The game's random generator (a Mersenne Twister at 0x821A1E28, seeded once
// at start-up from the clock by sub_82030010(state, seed)) seeded with a
// fixed number, so a benchmark game with no input plays out the same way
// every run (the game steps a fixed time per update: compared by update
// count, two builds meet the same situation).
extern "C" void __imp__sub_82030010(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82030010(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 == 0x821A1E28) {
    gw_log("test cheats: random seed %08X replaced by 12345\n", ctx.r4.u32);
    ctx.r4.u64 = 12345;
  }
  __imp__sub_82030010(ctx, base);
}

// The profile written from the live record: the real mask for the write.
extern "C" void __imp__sub_820E8A20(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820E8A20(PPCContext& ctx, uint8_t* base) {
  if (g_have_real) gw_store32(kMaskAddress, g_real_mask);
  __imp__sub_820E8A20(ctx, base);
  force_all();
}
#endif
