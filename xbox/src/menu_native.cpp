// The main menu's last item: Return to arcade on the Xbox 360, Return to dash
// here. Its text is changed on the staged disc (tools/stage_disc.py, the
// game's Text-en.loc); its action (guest 0x8208EFC0, the only menu callback
// the main menu builds with that item's text) is the in-game reset, the same
// command as the controller combo (gw_return_to_dash).
#include "runtime.h"

extern "C" void sub_8208EFC0(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  gw_return_to_dash("main menu: Return to dash");
}
