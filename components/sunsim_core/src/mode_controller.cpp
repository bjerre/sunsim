#include "sunsim/core/mode_controller.h"

#include "sunsim/ev/log.h"

namespace sunsim::core {

namespace {
constexpr const char *kTag = "MODE";
} // namespace

ModeController::ModeController(ev::Post post, int64_t double_click_window_us)
    : post_(std::move(post)), window_us_(double_click_window_us)
{
}

void ModeController::onPress(const ev::ButtonPressed &press)
{
    switch (press.press) {
    case ev::Press::Single:
        click_pending_ = false;
        enter(ev::Mode::Auto);
        break;
    case ev::Press::Double:
        click_pending_ = false;
        enter(ev::Mode::RedAlert);
        break;
    case ev::Press::Hold:
        click_pending_ = false;
        enter(ev::Mode::ManualOff);
        break;
    case ev::Press::Ambiguous:
        if (click_pending_ && press.at_us - last_click_us_ <= window_us_) {
            click_pending_ = false;
            enter(ev::Mode::RedAlert);
        } else {
            click_pending_ = true;
            last_click_us_ = press.at_us;
        }
        break;
    }
}

void ModeController::onTick(const ev::Tick &tick)
{
    if (click_pending_ && tick.uptime_us - last_click_us_ > window_us_) {
        click_pending_ = false;
        enter(ev::Mode::Auto);
    }
    if (mode_ == ev::Mode::RedAlert) {
        pulse();
    }
}

void ModeController::enter(ev::Mode mode)
{
    mode_ = mode;
    switch (mode) {
    case ev::Mode::Auto:
        SUNSIM_LOGI(kTag, "Resuming daylight program");
        break;
    case ev::Mode::ManualOff:
        SUNSIM_LOGI(kTag, "Manual off (press again to resume)");
        break;
    case ev::Mode::RedAlert:
        SUNSIM_LOGW(kTag, "RED ALERT");
        break;
    }
    post_(ev::ModeChanged{mode});
    if (mode == ev::Mode::RedAlert) {
        pulse(); // start immediately rather than on the next tick
    }
}

void ModeController::pulse()
{
    bright_ = !bright_;
    post_(ev::RedAlertPulse{bright_});
}

} // namespace sunsim::core
