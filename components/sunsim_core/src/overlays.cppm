export module sunsim.core:overlays;

import std.compat;
import sunsim.ev;
import :sky;

export namespace sunsim::core {

// A saturated colour: hue in degrees, saturation 0-254 (Zigbee's).
struct Shade {
    uint16_t hue_deg    = 0;
    uint8_t  saturation = 254;

    friend constexpr bool operator==(Shade, Shade) noexcept = default;
};

// A light breathing between bright and dim in one colour, like red alert.
struct Pulse {
    Shade   shade;
    uint8_t fade_seconds = 1;   // each fade, bright to dim or back: a cycle is twice this
    double  strength     = 1.0; // the top of the pulse, as a fraction of the light's peak

    friend constexpr bool operator==(const Pulse &, const Pulse &) noexcept = default;
};

// What the weather shows on `role`, if anything; pure. `lit`: the program
// has the light on. On the alert lights, a warning shows in daylight and off
// modes whether lit or not (it's an alarm); an advisory or watch only on a
// lit light in daylight mode. Failing that, on the precipitation lights,
// rain or snow shows in daylight mode: dimmer if the light is otherwise off.
[[nodiscard]] std::optional<Pulse> weatherPulse(ev::Role role, bool lit, ev::Mode mode,
                                                const ev::ProgramConfig &config, ev::AlertLevel alert,
                                                const std::optional<ev::Conditions> &now) noexcept;

// The stage between the running program and LightOutput: passes each
// LightTarget through as the OutputTarget, except on lights the weather has
// taken over (weatherPulse), which it pulses itself, a fade per Tick or
// slower. When the weather lets a light go, the program's latest target for
// it goes out again, with a quick transition. The weather is re-checked
// every Tick, so a change shows within a second.
class Overlays {
public:
    explicit Overlays(ev::Post post);

    void onTarget(const ev::LightTarget &e);
    void onTick(const ev::Tick &e);
    void onModeChanged(const ev::ModeChanged &e) { mode_ = e.mode; }
    void onConfig(const ev::ConfigChanged &e);
    void onConditions(const ev::ConditionsUpdated &e) { weather_.onConditions(e); }
    void onAlerts(const ev::AlertsUpdated &e) { weather_.onAlerts(e); }

private:
    struct Light {
        std::optional<ev::LightTarget> base;  // the program's latest
        std::optional<Pulse>           pulse; // shown instead, while the weather has the light
    };

    void show(ev::Role role, const Pulse &pulse, bool bright);
    void release(ev::Role role);

    ev::Post           post_;
    Weather            weather_;
    ev::Mode           mode_ = ev::Mode::Daylight;
    ev::ProgramConfig  config_{};
    ev::Peaks          peaks_      = ev::kDefaultPeaks;
    ev::MinLevels      min_levels_ = ev::kDefaultMinLevels;
    ev::PerRole<Light> lights_{};
    uint32_t           ticks_ = 0;
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
// Quick, for handing a light back to its program.
constexpr ev::Transition kReleaseTransition = std::chrono::seconds{2};
constexpr ev::Transition kColorTransition{2}; // 0.2 s

// The bottom of a pulse, as a fraction of its top.
constexpr double kDimFraction = 0.2;
// Rain or snow on a light the program has off: a glow, not the full pulse.
constexpr double kUnlitStrength = 0.35;

// Advisory yellow, watch orange, warning red; faster as it gets worse.
constexpr Pulse alertPulse(ev::AlertLevel level) noexcept
{
    switch (level) {
    case ev::AlertLevel::Advisory:
        return {.shade = {50, 254}, .fade_seconds = 3};
    case ev::AlertLevel::Watch:
        return {.shade = {28, 254}, .fade_seconds = 2};
    case ev::AlertLevel::Warning:
    case ev::AlertLevel::None:
        break;
    }
    return {.shade = {0, 254}, .fade_seconds = 1};
}

// Blues for water, paler and slower for snow; violet for freezing rain,
// purple and quick for thunder. Heavier falls pulse brighter.
constexpr Pulse precipPulse(ev::Precip precip, ev::Intensity intensity) noexcept
{
    const double strength = intensity == ev::Intensity::Heavy ? 1.0 : intensity == ev::Intensity::Moderate ? 0.8 : 0.6;
    switch (precip) {
    case ev::Precip::Drizzle:
        return {.shade = {205, 170}, .fade_seconds = 3, .strength = strength};
    case ev::Precip::FreezingRain:
        return {.shade = {280, 230}, .fade_seconds = 2, .strength = strength};
    case ev::Precip::Snow:
        return {.shade = {195, 150}, .fade_seconds = 3, .strength = strength};
    case ev::Precip::Thunderstorm:
        return {.shade = {270, 254}, .fade_seconds = 1, .strength = strength};
    case ev::Precip::Rain:
    case ev::Precip::None:
        break;
    }
    return {.shade = {228, 254}, .fade_seconds = 2, .strength = strength};
}

constexpr uint8_t zigbeeHue(uint16_t degrees) noexcept
{
    return static_cast<uint8_t>((degrees % 360) * 254 / 360);
}
} // namespace

std::optional<Pulse> weatherPulse(ev::Role role, bool lit, ev::Mode mode, const ev::ProgramConfig &config,
                                  ev::AlertLevel alert, const std::optional<ev::Conditions> &now) noexcept
{
    if (!config.weather_enabled || !ev::isBulb(role)) {
        return std::nullopt;
    }
    const bool daylight = mode == ev::Mode::Daylight;
    if (alert != ev::AlertLevel::None && ev::contains(config.alert_lights, role)) {
        const bool shown = alert == ev::AlertLevel::Warning ? daylight || mode == ev::Mode::Off : daylight && lit;
        if (shown) {
            return alertPulse(alert);
        }
    }
    if (daylight && now && now->precip != ev::Precip::None && ev::contains(config.precip_lights, role)) {
        Pulse pulse = precipPulse(now->precip, now->intensity);
        if (!lit) {
            pulse.strength *= kUnlitStrength;
        }
        return pulse;
    }
    return std::nullopt;
}

Overlays::Overlays(ev::Post post) : post_(std::move(post)) {}

void Overlays::onConfig(const ev::ConfigChanged &e)
{
    config_     = e.config;
    peaks_      = e.peaks;
    min_levels_ = e.min_levels;
}

void Overlays::onTarget(const ev::LightTarget &e)
{
    if (!ev::isBulb(e.role)) {
        return;
    }
    Light &light = lights_[e.role];
    light.base   = e;
    if (!light.pulse) {
        post_(ev::OutputTarget{e});
    }
}

void Overlays::onTick(const ev::Tick &e)
{
    ++ticks_;
    const ev::AlertLevel                alert = weather_.alertLevel(e.at);
    const std::optional<ev::Conditions> now   = weather_.conditions(e.at);
    for (const ev::Role role : ev::bulbs()) {
        Light     &light = lights_[role];
        const bool lit   = light.base && light.base->state.on;
        const auto pulse = weatherPulse(role, lit, mode_, config_, alert, now);
        if (!pulse) {
            if (light.pulse) {
                release(role);
            }
            continue;
        }
        // Every pulse fades on the same beat, so lights with the same
        // tempo breathe together.
        const bool changed = pulse != light.pulse;
        light.pulse        = pulse;
        if (changed || ticks_ % pulse->fade_seconds == 0) {
            show(role, *pulse, (ticks_ / pulse->fade_seconds) % 2 == 0);
        }
    }
}

void Overlays::show(ev::Role role, const Pulse &pulse, bool bright)
{
    const double  peak   = ev::kMaxLevel * peaks_[role] / 100.0 * pulse.strength;
    const uint8_t top    = std::max(min_levels_[role], static_cast<uint8_t>(std::max(1L, std::lround(peak))));
    const uint8_t bottom = std::clamp(static_cast<uint8_t>(std::lround(top * kDimFraction)), min_levels_[role], top);

    ev::OutputTarget out;
    out.role             = role;
    out.state            = {.on         = true,
                            .level      = bright ? top : bottom,
                            .color      = ev::ColorMode::HueSat,
                            .hue        = zigbeeHue(pulse.shade.hue_deg),
                            .saturation = pulse.shade.saturation};
    out.level_transition = ev::Transition{static_cast<uint16_t>(pulse.fade_seconds * 10 - 1)}; // nearly the whole fade
    out.color_transition = kColorTransition;
    post_(out);
}

void Overlays::release(ev::Role role)
{
    Light &light = lights_[role];
    light.pulse.reset();
    ev::OutputTarget out;
    if (light.base) {
        static_cast<ev::LightTarget &>(out) = *light.base;
    }
    out.role             = role;
    out.level_transition = kReleaseTransition;
    out.color_transition = kReleaseTransition;
    post_(out);
}

} // namespace sunsim::core
