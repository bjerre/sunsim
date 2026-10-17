export module sunsim.core:mode_controller;

import std.compat;
import sunsim.ev;

export namespace sunsim::core {

// Which program runs (ProgramHost runs it): the console can ask for one
// (ModeRequested: `tune`), and the button's presses choose:
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

    void on(const ev::ButtonPressed &press) { presses_.deliver(press, press.at); }
    void on(const ev::ModeRequested &e) { enter(e.mode); } // the console's `tune`
    void on(const ev::Tick &tick) { timer_.tick(tick.at); }

private:
    ev::Flow run();
    void     enter(ev::Mode mode);

    ev::Post                     post_;
    std::chrono::milliseconds    window_;
    ev::Timer                    timer_;
    ev::Inbox<ev::ButtonPressed> presses_{timer_};
    ev::Flow                     flow_; // last: run() starts once the rest exist
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
constexpr const char *kTag = "MODE";

// The mode a definite (non-Ambiguous) press selects.
ev::Mode modeFor(ev::Press press) noexcept
{
    switch (press) {
    case ev::Press::Double:
        return ev::Mode::RedAlert;
    case ev::Press::Hold:
        return ev::Mode::Off;
    case ev::Press::Single:
    case ev::Press::Ambiguous:
        break;
    }
    return ev::Mode::Daylight;
}
} // namespace

ModeController::ModeController(ev::Post post, std::chrono::milliseconds double_click_window)
    : post_(std::move(post)), window_(double_click_window), flow_(run())
{
}

ev::Flow ModeController::run()
{
    for (;;) {
        const ev::ButtonPressed press = co_await presses_.next();
        if (press.press != ev::Press::Ambiguous) {
            enter(modeFor(press.press));
            continue;
        }
        const auto second = co_await presses_.next(press.at + window_);
        if (!second) {
            enter(ev::Mode::Daylight); // the window passed: it was a single press
        } else if (second->press == ev::Press::Ambiguous) {
            enter(ev::Mode::RedAlert); // two within the window: a double press
        } else {
            enter(modeFor(second->press)); // a definite press wins
        }
    }
}

void ModeController::enter(ev::Mode mode)
{
    switch (mode) {
    case ev::Mode::Daylight:
        ev::log::info(kTag, "Resuming daylight program");
        break;
    case ev::Mode::Off:
        ev::log::info(kTag, "Manual off (press again to resume)");
        break;
    case ev::Mode::RedAlert:
        ev::log::warn(kTag, "RED ALERT");
        break;
    case ev::Mode::Tuning:
        ev::log::info(kTag, "Tuning: every bulb steady at its minimum level (`tune`; press to resume)");
        break;
    }
    post_(ev::ModeChanged{mode});
}

} // namespace sunsim::core
