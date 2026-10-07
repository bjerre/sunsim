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
    b.sent_state.reset();
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
            if (usable(b)) {
                ev::SetLight s = to(b);
                s.power        = false;
                post_(s);
                b.sent_valid = true;
                b.sent_on    = false;
            } else {
                b.sent_valid = false; // switch it off once it's back
            }
            b.sent_state.reset();
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
        b.sent_valid = false; // force a clean re-sync once we leave red alert
        b.sent_state.reset();
        if (!usable(b)) {
            continue;
        }
        ev::SetLight s        = to(b);
        s.hue_sat             = std::pair<uint8_t, uint8_t>{0, 254};
        s.color_transition_ds = kRedAlertColorDs;
        s.level               = e.bright ? kRedAlertBright : kRedAlertDim;
        s.level_transition_ds = kRedAlertFadeDs;
        post_(s);
    }
}

void LightOutput::onTarget(const ev::LightTarget &e)
{
    if (mode_ != ev::Mode::Auto || !ev::isBulb(e.role)) {
        return;
    }
    Bulb &b = bulbs_[ev::indexOf(e.role)];
    if (!usable(b)) {
        return;
    }

    if (e.state.on) {
        const ev::LightState *prev = (b.sent_valid && b.sent_on && b.sent_state) ? &*b.sent_state : nullptr;
        ev::SetLight          s    = to(b);
        if (!prev) {
            s.power = true;
        }
        if (!prev || prev->level != e.state.level) {
            s.level               = e.state.level;
            s.level_transition_ds = e.transition_ds;
        }
        const bool color_changed = !prev || prev->use_xy != e.state.use_xy ||
                                   (e.state.use_xy ? (prev->color_x != e.state.color_x || prev->color_y != e.state.color_y)
                                                   : prev->mireds != e.state.mireds);
        if (color_changed) {
            if (e.state.use_xy) {
                s.xy = std::pair<float, float>{e.state.color_x, e.state.color_y};
            } else {
                s.mireds = e.state.mireds;
            }
            s.color_transition_ds = e.transition_ds;
        }
        if (s.power || s.level || s.mireds || s.xy) {
            post_(s);
        }
        b.sent_state = e.state;
    } else {
        if (!b.sent_valid || b.sent_on) {
            ev::SetLight s = to(b);
            s.power        = false;
            post_(s);
        }
        b.sent_state.reset();
    }
    b.sent_valid = true;
    b.sent_on    = e.state.on;
}

void LightOutput::onReachability(const ev::ReachabilityChanged &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b     = bulbs_[ev::indexOf(e.role)];
    b.reachable = e.reachable;
    if (!e.reachable) {
        b.sent_valid = false; // unknown state when it's back: resend everything
        b.sent_state.reset();
    }
}

} // namespace sunsim::core
