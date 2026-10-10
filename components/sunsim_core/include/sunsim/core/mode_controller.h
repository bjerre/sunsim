#pragma once

#include <cstdint>

#include "sunsim/ev/events.h"
#include "sunsim/ev/flow.h"

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

    void onPress(const ev::ButtonPressed &press) { presses_.deliver(press, press.at_us); }
    void onTick(const ev::Tick &tick) { presses_.tick(tick.uptime_us); }

private:
    ev::Flow run();
    void     enter(ev::Mode mode);

    ev::Post                       post_;
    int64_t                        window_us_;
    ev::Mailbox<ev::ButtonPressed> presses_;
    ev::Flow                       flow_; // last: run() starts once the rest exist
};

} // namespace sunsim::core
