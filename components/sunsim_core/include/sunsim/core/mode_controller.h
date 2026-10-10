#pragma once

#include <cstdint>

#include "sunsim/ev/events.h"

namespace sunsim::core {

// Which program the button's presses choose (ProgramHost runs it):
//   single -> daylight                        (ModeChanged{Daylight})
//   double -> red alert                       (ModeChanged{RedAlert})
//   hold   -> all lights off                  (ModeChanged{Off})
// Press::Ambiguous (an On/Toggle only seen as an attribute change, from
// buttons that use the bound receiver endpoint) is resolved by timing: a
// second one within the double-click window is a double press, otherwise
// it's a single press once the window has passed (on the next Tick).
class ModeController {
public:
    explicit ModeController(ev::Post post, int64_t double_click_window_us = 450'000);

    void onPress(const ev::ButtonPressed &press);
    void onTick(const ev::Tick &tick);

private:
    void enter(ev::Mode mode);

    ev::Post post_;
    int64_t  window_us_;
    bool     click_pending_ = false;
    int64_t  last_click_us_ = 0;
};

} // namespace sunsim::core
