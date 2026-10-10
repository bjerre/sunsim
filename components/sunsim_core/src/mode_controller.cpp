module;

#include "sunsim/ev/events.h"
#include "sunsim/ev/flow.h"
#include "sunsim/ev/log.h"

module sunsim.core;

import std;

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
        SUNSIM_LOGI(kTag, "Resuming daylight program");
        break;
    case ev::Mode::Off:
        SUNSIM_LOGI(kTag, "Manual off (press again to resume)");
        break;
    case ev::Mode::RedAlert:
        SUNSIM_LOGW(kTag, "RED ALERT");
        break;
    }
    post_(ev::ModeChanged{mode});
}

} // namespace sunsim::core
