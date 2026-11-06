// Xbox stand-in for rex/chrono/clock.h.
#pragma once

#include <rex/types.h>

namespace rex::chrono {
struct Clock {
  // Guest timebase (Xbox 360: 49.875 MHz), derived from the host clock.
  static u64 QueryGuestTickCount();
};
}  // namespace rex::chrono
