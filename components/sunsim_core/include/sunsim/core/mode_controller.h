#pragma once

#include <chrono>

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
    explicit ModeController(ev::Post post, std::chrono::milliseconds double_click_window = std::chrono::milliseconds{450});

    void onPress(const ev::ButtonPressed &press) { presses_.deliver(press, ev::Uptime{press.at_us}); }
    void onTick(const ev::Tick &tick) { timer_.tick(ev::Uptime{tick.uptime_us}); }

private:
    ev::Flow run();
    void     enter(ev::Mode mode);

    ev::Post                     post_;
    ev::Uptime                   window_;
    ev::Timer                    timer_;
    ev::Inbox<ev::ButtonPressed> presses_{timer_};
    ev::Flow                     flow_; // last: run() starts once the rest exist
};

} // namespace sunsim::core
