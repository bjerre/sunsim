export module sunsim.core:overlays;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::core {

// A saturated color: hue in degrees, saturation 0-254 (Zigbee's).
struct Shade {
    uint16_t hue_deg    = 0;
    uint8_t  saturation = 254;

    friend constexpr bool operator==(Shade, Shade) noexcept = default;
};

// A light breathing between bright and dim in one color, like red alert.
struct Pulse {
    Shade   shade;
    uint8_t fade_seconds = 1;   // each fade, bright to dim or back: a cycle is twice this
    double  strength     = 1.0; // the top of the pulse, as a fraction of the light's peak

    friend constexpr bool operator==(const Pulse &, const Pulse &) noexcept = default;
};

// What the sky shows on `role`, if anything; pure. Only on the weather
// lights (`lights`: the zones' declared ones). `lit`: the program has the
// light on; `mode`: its zone's. On the alert lights, a warning shows in
// daylight and off modes whether lit or not (it's an alarm); an advisory or
// watch only on a lit light in daylight mode. Failing that, on the
// precipitation lights, rain or snow shows in daylight mode: dimmer if the
// light is otherwise off.
[[nodiscard]] std::optional<Pulse> weatherPulse(ev::Role role, bool lit, ev::Mode mode, const ev::ProgramConfig &config,
                                                const ev::Sky           &sky,
                                                const ev::WeatherLights &lights = ev::kWeatherLights) noexcept;

// The stage between the running program and LightOutput: passes each
// LightTarget through as the OutputTarget, except on lights the weather has
// taken over (weatherPulse), which it pulses itself: a fade each beat of
// the Ticks' time (fade_seconds), whenever the Ticks come. When the weather lets a light go, the program's latest target for
// it goes out again, with a quick transition. Re-checked when the sky
// changes (SkyChanged: at once) and every Tick (the mode, the program's
// lights, the pulse's beat).
class Overlays {
public:
    explicit Overlays(ev::Post post);

    void on(const ev::LightTarget &e);
    void on(const ev::Tick &e);
    void on(const ev::ModeChanged &e) { modes_[e.zone] = e.mode; }
    void on(const ev::ConfigChanged &e);
    void on(const ev::SkyChanged &e);

private:
    struct Light {
        std::optional<ev::LightTarget> base;  // the program's latest
        std::optional<Pulse>           pulse; // shown instead, while the weather has the light
        std::optional<int64_t>         beat;  // the pulse's beat last shown
    };

    void reconcile();
    void show(ev::Role role, const Pulse &pulse, bool bright);
    void release(ev::Role role);

    ev::Post              post_;
    ev::Sky               sky_{};
    ev::PerZone<ev::Mode> modes_{}; // each zone's; Daylight, the first Mode, to start
    ev::ProgramConfig     config_{};
    ev::Peaks             peaks_      = ev::kDefaultPeaks;
    ev::MinLevels         min_levels_ = ev::kDefaultMinLevels;
    ev::PerRole<Light>    lights_{};
    ev::Instant           now_{}; // the latest Tick's
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
                                  const ev::Sky &sky, const ev::WeatherLights &lights) noexcept
{
    const ev::AlertLevel                 alert = sky.alert();
    const std::optional<ev::Conditions> &now   = sky.conditions;
    if (!config.weather_enabled) {
        return std::nullopt;
    }
    const bool daylight = mode == ev::Mode::Daylight;
    if (alert != ev::AlertLevel::None && ev::contains(lights.alerts, role)) {
        const bool shown = alert == ev::AlertLevel::Warning ? daylight || mode == ev::Mode::Off : daylight && lit;
        if (shown) {
            return alertPulse(alert);
        }
    }
    if (daylight && now && now->precip != ev::Precip::None && ev::contains(lights.precip, role)) {
        Pulse pulse = precipPulse(now->precip, now->intensity);
        if (!lit) {
            pulse.strength *= kUnlitStrength;
        }
        return pulse;
    }
    return std::nullopt;
}

Overlays::Overlays(ev::Post post) : post_(std::move(post)) {}

void Overlays::on(const ev::ConfigChanged &e)
{
    config_     = e.config;
    peaks_      = e.peaks;
    min_levels_ = e.min_levels;
}

void Overlays::on(const ev::LightTarget &e)
{
    if (!ev::isLight(e.role)) {
        return;
    }
    Light &light = lights_[e.role];
    light.base   = e;
    if (!light.pulse) {
        post_(ev::OutputTarget{e});
    }
}

void Overlays::on(const ev::Tick &e)
{
    now_ = e.at;
    reconcile();
}

void Overlays::on(const ev::SkyChanged &e)
{
    sky_ = e.sky;
    reconcile();
}

// Each light to what the sky shows on it now: a pulse started or changed,
// one fading on this beat, or one let go.
void Overlays::reconcile()
{
    for (const ev::Role role : ev::lights()) {
        Light     &light = lights_[role];
        const bool lit   = light.base && light.base->state.on;
        const auto pulse = weatherPulse(role, lit, modes_[*ev::kZoneOf[role]], config_, sky_);
        if (!pulse) {
            if (light.pulse) {
                release(role);
            }
            continue;
        }
        // Every pulse beats by the same clock, so lights with the same
        // tempo breathe together.
        const int64_t beat    = now_.time_since_epoch() / std::chrono::seconds{pulse->fade_seconds};
        const bool    changed = pulse != light.pulse;
        light.pulse           = pulse;
        if (changed || beat != light.beat) {
            light.beat = beat;
            show(role, *pulse, beat % 2 == 0);
        }
    }
}

void Overlays::show(ev::Role role, const Pulse &pulse, bool bright)
{
    const double  peak   = ev::kMaxLevel * peaks_[role] / 100.0 * pulse.strength;
    const uint8_t top    = std::max(min_levels_[role], static_cast<uint8_t>(std::max(1L, std::lround(peak))));
    const uint8_t bottom = std::clamp(static_cast<uint8_t>(std::lround(top * kDimFraction)), min_levels_[role], top);

    post_(ev::OutputTarget{{
        .role             = role,
        .state            = {.on    = true,
                             .level = bright ? top : bottom,
                             .color = ev::HueSat{zigbeeHue(pulse.shade.hue_deg), pulse.shade.saturation}},
        .level_transition = ev::Transition{static_cast<uint16_t>(pulse.fade_seconds * 10 - 1)}, // nearly the whole fade
        .color_transition = kColorTransition,
    }});
}

void Overlays::release(ev::Role role)
{
    Light &light = lights_[role];
    light.pulse.reset();
    light.beat.reset();
    // Back to the program's latest (off, if it hasn't sent one), quickly.
    ev::LightChange back = light.base.value_or(ev::LightTarget{{.role = role}});
    back.level_transition = back.color_transition = kReleaseTransition;
    post_(ev::OutputTarget{back});
}

} // namespace sunsim::core
