export module sunsim.core:light_output;

import std.compat;
import sunsim.ev;

export namespace sunsim::core {

// Turns what the bulbs *should* show (OutputTarget: the running program's,
// or the weather's, see Overlays) into the ZCL commands that get them there (SetLight). Only what
// changed since the last command is sent: On/Off on a change, level and
// colour when they move. Bulbs marked unreachable get nothing until they
// answer again (the registry then posts RefreshLights). Bulb addresses come
// from RoleAssigned/RoleCleared.
class LightOutput {
public:
    explicit LightOutput(ev::Post post);

    void on(const ev::RoleAssigned &e);
    void on(const ev::RoleCleared &e);
    void on(const ev::ReachabilityChanged &e);
    void on(const ev::OutputTarget &e);

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
    ev::PerRole<Bulb>                bulbs_{}; // bulbs only
};

} // namespace sunsim::core

namespace sunsim::core {

LightOutput::LightOutput(ev::Post post) : post_(std::move(post)) {}

void LightOutput::on(const ev::RoleAssigned &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b      = bulbs_[e.role];
    b.known      = e.record.valid && e.record.endpoint != 0;
    b.short_addr = e.record.short_addr;
    b.endpoint   = e.record.endpoint;
    b.sent.reset(); // (re)paired or moved: resend everything next time
}

void LightOutput::on(const ev::RoleCleared &e)
{
    if (ev::isBulb(e.role)) {
        bulbs_[e.role] = {};
    }
}

void LightOutput::on(const ev::ReachabilityChanged &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b     = bulbs_[e.role];
    b.reachable = e.reachable;
    if (!e.reachable) {
        b.sent.reset(); // unknown state when it's back: resend everything
    }
}

void LightOutput::on(const ev::OutputTarget &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Bulb &b = bulbs_[e.role];
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
        if (!was_on || b.sent->color != e.state.color) {
            s.color            = e.state.color;
            s.color_transition = e.color_transition;
        }
    } else if (!b.sent || was_on) {
        s.power = false;
    }

    if (s.power || s.level || s.color) {
        post_(s);
    }
    b.sent = e.state;
}

} // namespace sunsim::core
