export module sunsim.core:light_output;

import std.compat;
import sunsim.ev;

export namespace sunsim::core {

// What a light with `caps` can show of `state`: a plug or a dimmable bulb
// at full level (its level isn't sent), a white bulb an xy color as its
// nearest color temperature. A color it can't show at all (a saturated one
// on a white bulb) stays in the state, so it diffs as a change, but
// commandsFor never sends it: the light keeps its color.
[[nodiscard]] ev::LightState fitted(ev::LightState state, ev::Caps caps) noexcept;

// The commands that take a light with `caps` from `sent` (nullopt: unknown,
// send everything) to `to`: only what changed and the light takes; nullopt
// if that's nothing. Its address is left for the caller.
[[nodiscard]] std::optional<ev::SetLight> commandsFor(const std::optional<ev::LightState> &sent,
                                                      const ev::LightChange &to, ev::Caps caps) noexcept;

// Turns what the lights *should* show (OutputTarget: the running program's,
// or the weather's, see Overlays) into the ZCL commands that get them there
// (SetLight), fitted to each light's kind (ev::caps). Only what changed
// since the last command is sent: On/Off on a change, level and color when
// they move. Lights marked unreachable get nothing until they answer again
// (the registry then posts RefreshLights). Their addresses come from
// RoleAssigned/RoleCleared.
class LightOutput {
public:
    explicit LightOutput(ev::Post post);

    void on(const ev::RoleAssigned &e);
    void on(const ev::RoleCleared &e);
    void on(const ev::ReachabilityChanged &e);
    void on(const ev::OutputTarget &e);

private:
    struct Light {
        bool                          known      = false; // paired with a known endpoint
        uint16_t                      short_addr = 0;
        uint8_t                       endpoint   = 0;
        bool                          reachable  = true;
        std::optional<ev::LightState> sent; // what we last told it; nullopt = unknown, resend everything
    };
    [[nodiscard]] static bool usable(const Light &l) noexcept { return l.known && l.reachable; }

    ev::Post           post_;
    ev::PerRole<Light> lights_{}; // lights only
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {

// The color temperature nearest an xy chromaticity (McCamy's cubic: within
// a few kelvin from ~2000 to 12500K, close enough below for a white bulb,
// which clamps to its own range anyway).
ev::Mireds nearestMireds(ev::Xy xy) noexcept
{
    const double n      = (xy.x - 0.3320) / (0.1858 - xy.y);
    const double kelvin = std::max(1000.0, ((449.0 * n + 3525.0) * n + 6823.3) * n + 5520.33);
    return {static_cast<uint16_t>(std::lround(1e6 / kelvin))};
}

// Whether a light with `caps` can be sent `color`.
bool takes(const ev::Color &color, ev::Caps caps) noexcept
{
    return std::holds_alternative<ev::Mireds>(color) ? caps.mireds : caps.color;
}

} // namespace

ev::LightState fitted(ev::LightState state, ev::Caps caps) noexcept
{
    if (!caps.level) {
        state.level = ev::kMaxLevel;
    }
    if (const auto *xy = std::get_if<ev::Xy>(&state.color); xy && !caps.color && caps.mireds) {
        state.color = nearestMireds(*xy);
    }
    return state;
}

std::optional<ev::SetLight> commandsFor(const std::optional<ev::LightState> &sent, const ev::LightChange &to,
                                        ev::Caps caps) noexcept
{
    ev::SetLight s;
    const bool   was_on = sent && sent->on;
    if (to.state.on) {
        if (!was_on) {
            s.power = true;
        }
        if (caps.level && (!was_on || sent->level != to.state.level)) {
            s.level            = to.state.level;
            s.level_transition = to.level_transition;
        }
        if (takes(to.state.color, caps) && (!was_on || sent->color != to.state.color)) {
            s.color            = to.state.color;
            s.color_transition = to.color_transition;
        }
    } else if (!sent || was_on) {
        s.power = false;
    }
    if (!s.power && !s.level && !s.color) {
        return std::nullopt;
    }
    return s;
}

LightOutput::LightOutput(ev::Post post) : post_(std::move(post)) {}

void LightOutput::on(const ev::RoleAssigned &e)
{
    if (!ev::isLight(e.role)) {
        return;
    }
    Light &l     = lights_[e.role];
    l.known      = e.record.valid && e.record.endpoint != 0;
    l.short_addr = e.record.short_addr;
    l.endpoint   = e.record.endpoint;
    l.sent.reset(); // (re)paired or moved: resend everything next time
}

void LightOutput::on(const ev::RoleCleared &e)
{
    if (ev::isLight(e.role)) {
        lights_[e.role] = {};
    }
}

void LightOutput::on(const ev::ReachabilityChanged &e)
{
    if (!ev::isLight(e.role)) {
        return;
    }
    Light &l    = lights_[e.role];
    l.reachable = e.reachable;
    if (!e.reachable) {
        l.sent.reset(); // unknown state when it's back: resend everything
    }
}

void LightOutput::on(const ev::OutputTarget &e)
{
    if (!ev::isLight(e.role)) {
        return;
    }
    Light &l = lights_[e.role];
    if (!usable(l)) {
        return;
    }
    const ev::Caps  caps = ev::caps(e.role);
    ev::LightChange to   = e;
    to.state             = fitted(e.state, caps);
    if (auto s = commandsFor(l.sent, to, caps)) {
        s->short_addr = l.short_addr;
        s->endpoint   = l.endpoint;
        post_(*s);
    }
    l.sent = to.state;
}

} // namespace sunsim::core
