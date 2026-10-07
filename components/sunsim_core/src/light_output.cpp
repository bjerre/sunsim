#include "sunsim/core/light_output.h"

namespace sunsim::core {

namespace {
// Red alert pulses like TNG's red-alert lighting: a slow, continuous glow
// rather than a strobe. A pulse arrives every 1 s Tick (a 2 s bright/dim
// cycle, ~0.5 Hz) and each fade takes nearly the whole second.
constexpr uint16_t kRedAlertFadeDs   = 9; // 0.9 s
constexpr uint16_t kRedAlertColorDs  = 2;
constexpr uint8_t  kRedAlertBright   = 254;
constexpr uint8_t  kRedAlertDim      = 30;
} // namespace

LightOutput::LightOutput(ev::Post post) : post_(std::move(post)) {}

ev::SetLight LightOutput::to(const Bulb &b) const
{
    ev::SetLight s;
    s.short_addr = b.short_addr;
    s.endpoint   = b.endpoint;
    return s;
}

void LightOutput::onRoleAssigned(const ev::RoleAssigned &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b      = bulbs_[ev::indexOf(e.role)];
    b.known      = e.record.valid && e.record.endpoint != 0;
    b.short_addr = e.record.short_addr;
    b.endpoint   = e.record.endpoint;
    b.sent_valid = false; // (re)paired or moved: resend everything next time
}

void LightOutput::onRoleCleared(const ev::RoleCleared &e)
{
    if (ev::isBulb(e.role)) {
        bulbs_[ev::indexOf(e.role)] = {};
    }
}

void LightOutput::onModeChanged(const ev::ModeChanged &e)
{
    mode_ = e.mode;
    switch (mode_) {
    case ev::Mode::Auto:
        post_(ev::RefreshLights{});
        break;
    case ev::Mode::ManualOff:
        for (Bulb &b : bulbs_) {
            if (b.known) {
                ev::SetLight s = to(b);
                s.power        = false;
                post_(s);
                b.sent_valid = true;
                b.sent_on    = false;
            }
        }
        break;
    case ev::Mode::RedAlert:
        break; // RedAlertPulses drive it
    }
}

void LightOutput::onRedAlertPulse(const ev::RedAlertPulse &e)
{
    if (mode_ != ev::Mode::RedAlert) {
        return;
    }
    for (Bulb &b : bulbs_) {
        if (!b.known) {
            continue;
        }
        ev::SetLight s        = to(b);
        s.hue_sat             = std::pair<uint8_t, uint8_t>{0, 254};
        s.color_transition_ds = kRedAlertColorDs;
        s.level               = e.bright ? kRedAlertBright : kRedAlertDim;
        s.level_transition_ds = kRedAlertFadeDs;
        post_(s);
        b.sent_valid = false; // force a clean re-sync once we leave red alert
    }
}

void LightOutput::onTarget(const ev::LightTarget &e)
{
    if (mode_ != ev::Mode::Auto || !ev::isBulb(e.role)) {
        return;
    }
    Bulb &b = bulbs_[ev::indexOf(e.role)];
    if (!b.known) {
        return;
    }

    if (e.state.on) {
        ev::SetLight s = to(b);
        if (!b.sent_valid || !b.sent_on) {
            s.power = true;
        }
        s.level               = e.state.level;
        s.level_transition_ds = e.transition_ds;
        if (e.state.use_xy) {
            s.xy = std::pair<float, float>{e.state.color_x, e.state.color_y};
        } else {
            s.mireds = e.state.mireds;
        }
        s.color_transition_ds = e.transition_ds;
        post_(s);
    } else if (!b.sent_valid || b.sent_on) {
        ev::SetLight s = to(b);
        s.power        = false;
        post_(s);
    }
    b.sent_valid = true;
    b.sent_on    = e.state.on;
}

} // namespace sunsim::core
