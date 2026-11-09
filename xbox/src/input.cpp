// Controller input for XamInputGetState: Xbox pads through SDL (nxdk's USB
// stack), plus a test-only remote pad that tools/xemu_pad.py writes through
// XEMU's GDB stub.
#include "runtime.h"

#include <SDL.h>
#include <windows.h>

// Test pad, one record per player: [0] magic, [1] XInput button mask,
// [2] left stick X | Y<<16, [3] right stick X | Y<<16, [4] left | right
// trigger<<16 (0..255). Sticks are int16, Y positive up as in XInput. A
// record is ignored until the magic word is written.
constexpr u32 kRemotePadMagic = 0x44415047;  // "GPAD"
extern "C" volatile u32 gw_remote_pad[4][5];
volatile u32 gw_remote_pad[4][5];

namespace {

constexpr int kPads = 4;
SDL_GameController* g_controllers[kPads];
bool g_sdl_ok;

// The in-game reset, as Perfect Dark X: LT+RT+Back+Black held together on
// any controller returns to the dashboard (gw_return_to_dash). Read from the
// controller's fixed physical layout (nxdk's joystick: axes 2 and 5 the
// triggers, buttons 6 Back and 5 Black), not through any button mapping.
void check_reset_combo() {
  for (int i = 0; i < kPads; ++i) {
    if (!g_controllers[i]) continue;
    SDL_Joystick* joy = SDL_GameControllerGetJoystick(g_controllers[i]);
    if (!joy) continue;
    if (SDL_JoystickGetAxis(joy, 2) > 16384 && SDL_JoystickGetAxis(joy, 5) > 16384 && SDL_JoystickGetButton(joy, 6) &&
        SDL_JoystickGetButton(joy, 5)) {
      for (auto& c : g_controllers)
        if (c) SDL_GameControllerRumble(c, 0, 0, 0);
      gw_log("reset: player %d held LT+RT+Back+Black\n", i + 1);
      gw_return_to_dash("controller combo");
    }
  }
}

void update_controllers() {
  // The game reads the pad several times a frame; SDL's update also polls
  // the USB hubs, so it runs at most every 8 ms (reads in between see the
  // last state).
  static DWORD last = 0;
  const DWORD now = GetTickCount();
  if (last && now - last < 8) return;
  last = now;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      for (auto& c : g_controllers) {
        if (c) continue;
        c = SDL_GameControllerOpen(event.cdevice.which);
        gw_log("input: controller connected\n");
        break;
      }
    } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      for (auto& c : g_controllers) {
        if (c && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == event.cdevice.which) {
          SDL_GameControllerClose(c);
          c = nullptr;
        }
      }
    }
  }
  SDL_GameControllerUpdate();
  check_reset_combo();
}

int16_t axis(SDL_GameController* c, SDL_GameControllerAxis a, bool flip) {
  int v = SDL_GameControllerGetAxis(c, a);
  if (flip) v = -1 - v;  // SDL Y grows down; XInput Y grows up
  return (int16_t)v;
}

}  // namespace

void gw_input_init() {
  g_sdl_ok = SDL_Init(SDL_INIT_GAMECONTROLLER) == 0;
  if (!g_sdl_ok) gw_log("input: SDL_Init failed: %s\n", SDL_GetError());
}

// Fills an XINPUT_GAMEPAD (12 bytes, guest byte order) for a user; false
// when no pad is connected for that user.
bool gw_input_read(u32 user, u32 gamepad) {
  if (user >= kPads) return false;
  volatile u32* remote = gw_remote_pad[user];
  if (remote[0] == kRemotePadMagic) {
    gw_store16(gamepad + 0, (u16)remote[1]);
    gw_store8(gamepad + 2, (u8)remote[4]);
    gw_store8(gamepad + 3, (u8)(remote[4] >> 16));
    gw_store16(gamepad + 4, (u16)remote[2]);
    gw_store16(gamepad + 6, (u16)(remote[2] >> 16));
    gw_store16(gamepad + 8, (u16)remote[3]);
    gw_store16(gamepad + 10, (u16)(remote[3] >> 16));
    return true;
  }
  if (!g_sdl_ok) return false;
  if (user == 0) {
    const u64 start = gw_wait_begin();
    update_controllers();
    gw_wait_end(start, 0, "input: SDL controller update");
  }
  SDL_GameController* c = g_controllers[user];
  if (!c) return false;
  static const struct {
    SDL_GameControllerButton button;
    u16 mask;
  } kButtons[] = {
      {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0001},     {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0002},
      {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0004},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0008},
      {SDL_CONTROLLER_BUTTON_START, 0x0010},       {SDL_CONTROLLER_BUTTON_BACK, 0x0020},
      {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0040},   {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080},
      {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200},
      {SDL_CONTROLLER_BUTTON_A, 0x1000},           {SDL_CONTROLLER_BUTTON_B, 0x2000},
      {SDL_CONTROLLER_BUTTON_X, 0x4000},           {SDL_CONTROLLER_BUTTON_Y, 0x8000},
  };
  u16 buttons = 0;
  for (const auto& b : kButtons) {
    if (SDL_GameControllerGetButton(c, b.button)) buttons |= b.mask;
  }
  gw_store16(gamepad + 0, buttons);
  gw_store8(gamepad + 2, (u8)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7));
  gw_store8(gamepad + 3, (u8)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7));
  gw_store16(gamepad + 4, (u16)axis(c, SDL_CONTROLLER_AXIS_LEFTX, false));
  gw_store16(gamepad + 6, (u16)axis(c, SDL_CONTROLLER_AXIS_LEFTY, true));
  gw_store16(gamepad + 8, (u16)axis(c, SDL_CONTROLLER_AXIS_RIGHTX, false));
  gw_store16(gamepad + 10, (u16)axis(c, SDL_CONTROLLER_AXIS_RIGHTY, true));
  return true;
}
