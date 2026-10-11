export module sunsim.core:light_output;

import std.compat;
import sunsim.ev;

export namespace sunsim::core {

// Turns what the bulbs *should* show (LightTarget, from whichever program is
// running) into the ZCL commands that get them there (SetLight). Only what
// changed since the last command is sent: On/Off on a change, level and
// colour when they move. Bulbs marked unreachable get nothing until they
// answer again (the registry then posts RefreshLights). Bulb addresses come
// from RoleAssigned/RoleCleared.
class LightOutput {
public:
    explicit LightOutput(ev::Post post);

    void onRoleAssigned(const ev::RoleAssigned &e);
    void onRoleCleared(const ev::RoleCleared &e);
    void onReachability(const ev::ReachabilityChanged &e);
    void onTarget(const ev::LightTarget &e);

private:
    struct Bulb {
        bool                          known      = false; // paired with a known endpoint
        uint16_t                      short_addr = 0;
        uint8_t                       endpoint   = 0;
        bool                          reachable  = true;
        std::optional<ev::LightState> sent; // what we last told it; nullopt = unknown, resend everything
    };
    [[nodiscard]] static bool usable(const Bulb &b) noexcept { return b.known && b.reachable; }

    ev::Post                         post_;
    std::array<Bulb, ev::kBulbCount> bulbs_{};
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {

bool sameColor(const ev::LightState &a, const ev::LightState &b) noexcept
{
    if (a.color != b.color) {
        return false;
    }
    switch (a.color) {
    case ev::ColorMode::Temperature:
        return a.mireds == b.mireds;
    case ev::ColorMode::Xy:
        return a.color_x == b.color_x && a.color_y == b.color_y;
    case ev::ColorMode::HueSat:
        return a.hue == b.hue && a.saturation == b.saturation;
    }
    return false;
}

void setColor(ev::SetLight &s, const ev::LightState &state) noexcept
{
    switch (state.color) {
    case ev::ColorMode::Temperature:
        s.mireds = state.mireds;
        break;
    case ev::ColorMode::Xy:
        s.xy = std::pair<float, float>{state.color_x, state.color_y};
        break;
    case ev::ColorMode::HueSat:
        s.hue_sat = std::pair<uint8_t, uint8_t>{state.hue, state.saturation};
        break;
    }
}

} // namespace

LightOutput::LightOutput(ev::Post post) : post_(std::move(post)) {}

void LightOutput::onRoleAssigned(const ev::RoleAssigned &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b      = bulbs_[ev::indexOf(e.role)];
    b.known      = e.record.valid && e.record.endpoint != 0;
    b.short_addr = e.record.short_addr;
    b.endpoint   = e.record.endpoint;
    b.sent.reset(); // (re)paired or moved: resend everything next time
}

void LightOutput::onRoleCleared(const ev::RoleCleared &e)
{
    if (ev::isBulb(e.role)) {
        bulbs_[ev::indexOf(e.role)] = {};
    }
}

void LightOutput::onReachability(const ev::ReachabilityChanged &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b     = bulbs_[ev::indexOf(e.role)];
    b.reachable = e.reachable;
    if (!e.reachable) {
        b.sent.reset(); // unknown state when it's back: resend everything
    }
}

void LightOutput::onTarget(const ev::LightTarget &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b = bulbs_[ev::indexOf(e.role)];
    if (!usable(b)) {
        return;
    }

    ev::SetLight s;
    s.short_addr = b.short_addr;
    s.endpoint   = b.endpoint;

    const bool was_on = b.sent && b.sent->on;
    if (e.state.on) {
        if (!was_on) {
            s.power = true;
        }
        if (!was_on || b.sent->level != e.state.level) {
            s.level            = e.state.level;
            s.level_transition = e.level_transition;
        }
        if (!was_on || !sameColor(*b.sent, e.state)) {
            setColor(s, e.state);
            s.color_transition = e.color_transition;
        }
    } else if (!b.sent || was_on) {
        s.power = false;
    }

    if (s.power || s.level || s.mireds || s.xy || s.hue_sat) {
        post_(s);
    }
    b.sent = e.state;
}

} // namespace sunsim::core
