export module sunsim.core:daylight;

import std.compat;
import sunsim.ev;
import :solar;

export namespace sunsim::core {

using ev::LightState;

// CIE 1931 xy of a blackbody at `kelvin` (Kim et al. cubic spline
// approximation of the Planckian locus, valid 1667-25000K): for colours
// below most bulbs' ~2700K colour-temperature floor.
void planckianXy(double kelvin, float &x_out, float &y_out) noexcept;

// Astronomical daylight model for the wall bulbs: warm and dim at the real
// sunrise/sunset for a configured location, brightest and coolest (bluest)
// at the window's midpoint ("solar noon"); a cool-to-warm work light that
// tracks the work day rather than the sun (bright and cool in the morning,
// warm by evening). Both only run inside the active window
// (work days and hours, see Config). See :solar for the
// sunrise/sunset calculation and DESIGN.md for the full math.
class DaylightSchedule {
public:
    // All tunables live in ev::ProgramConfig (shared with settings/console).
    using Config = ev::ProgramConfig;

    constexpr DaylightSchedule() noexcept : config_{} {}
    explicit constexpr DaylightSchedule(Config config) noexcept : config_(config) {}

    // `time` carries the local civil UTC offset in effect (DST included;
    // see platform::Clock for how it's derived), which the sun's position
    // needs. wall_position: where the bulb sits along the (south) wall, +1 = east
    // end, 0 = middle, -1 = west end; drives the sun-direction dimming and
    // the amber sweep. cloud: the cloud cover now, 0 (clear) to 1 (overcast).
    [[nodiscard]] LightState wallState(const ev::TimeReading &time, double wall_position = 0.0,
                                       double cloud = 0.0) const noexcept;
    [[nodiscard]] LightState workState(const ev::TimeReading &time) const noexcept;

    // True inside the active window (lights may be on), including its fades.
    [[nodiscard]] bool inActiveWindow(const ev::TimeReading &time) const noexcept { return activeGate(time) > 0.0; }

    [[nodiscard]] const Config &config() const noexcept { return config_; }
    void setConfig(const Config &config) noexcept { config_ = config; }

private:
    // 0 = off (outside active days/hours), 1 = full curve, ramping between
    // the two across the fade at each edge of the active window.
    [[nodiscard]] double activeGate(const ev::TimeReading &time) const noexcept;

    Config config_;
};

} // namespace sunsim::core

namespace sunsim::core {

void planckianXy(double kelvin, float &x_out, float &y_out) noexcept
{
    const double t  = std::fmin(25000.0, std::fmax(1667.0, kelvin));
    const double t2 = t * t;
    const double t3 = t2 * t;
    double       x  = 0.0;
    if (t <= 4000.0) {
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    } else {
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    }
    const double x2 = x * x;
    const double x3 = x2 * x;
    double       y  = 0.0;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
    x_out = static_cast<float>(x);
    y_out = static_cast<float>(y);
}

double DaylightSchedule::activeGate(const ev::TimeReading &time) const noexcept
{
    const std::chrono::seconds now = time.sinceMidnight();
    if (!config_.active_days.contains(time.weekday()) || !config_.active.contains(now)) {
        return 0.0;
    }
    if (config_.fade <= std::chrono::minutes{0}) {
        return 1.0;
    }
    // How far into the fade at either edge, 0 to 1 and beyond.
    const auto fade_in  = std::chrono::duration<double>{now - config_.active.start} / config_.fade;
    const auto fade_out = std::chrono::duration<double>{config_.active.end - now} / config_.fade;
    return std::fmin(1.0, std::fmin(fade_in, fade_out));
}

LightState DaylightSchedule::wallState(const ev::TimeReading &time, double wall_position, double cloud) const noexcept
{
    // Clouds make the light diffuse: dimmer, cooler, no sun direction and
    // no golden glow, each in proportion to the cover.
    const double overcast = config_.clouds_enabled ? std::clamp(cloud, 0.0, 1.0) : 0.0;

    const double gate = activeGate(time);
    if (gate <= 0.0) {
        return {.on = false, .level = 0, .mireds = config_.warm_mireds};
    }

    const ev::Hours now{time.sinceMidnight()};
    const ev::Hours utc_offset{time.utc_offset};
    const int       day_of_year = time.dayOfYear();
    const SunTimes  sun = computeSunTimes(config_.latitude_deg, config_.longitude_deg, day_of_year, utc_offset);

    const double hour    = now.count();
    double       sunrise = sun.sunrise.count();
    double       sunset  = sun.sunset.count();
    if (sunset <= sunrise) {
        // The window spills past local midnight (near-polar summer): unwrap
        // it onto a single increasing scale rather than treating every hour
        // as "outside" it.
        sunset += 24.0;
    }
    double hour_adj = hour;
    if (hour_adj < sunrise) {
        hour_adj += 24.0;
    }

    if (hour_adj < sunrise || hour_adj > sunset) {
        return {.on = false, .level = 0, .mireds = config_.warm_mireds};
    }

    const double day_progress = (hour_adj - sunrise) / (sunset - sunrise);
    const double brightness   = std::sin(std::numbers::pi * day_progress); // 0 at edges, 1 at solar noon

    // Sun direction: sin(azimuth) is the sun's east-west position (+1 due
    // East, 0 due South, -1 due West). `away` is how far along the wall the
    // bulb is from it: 0 under the sun, 1 at the opposite end. Bulbs further
    // away dim, by up to sun_shift_percent.
    const SunPosition pos =
        computeSunPosition(config_.latitude_deg, config_.longitude_deg, day_of_year, now, utc_offset);
    const double sun_x     = std::sin(pos.azimuth_deg * std::numbers::pi / 180.0);
    const double away      = std::fabs(wall_position - sun_x) / 2.0;
    const double direction = 1.0 - (config_.sun_shift_percent / 100.0) * away * (1.0 - overcast);
    const double sky       = 1.0 - (config_.cloud_dim_percent / 100.0) * overcast;

    uint8_t level = static_cast<uint8_t>(std::lround(gate * direction * sky * brightness * config_.max_level));
    if (level < config_.min_level) {
        level = config_.min_level;
    }

    const double sun_mireds = config_.warm_mireds - brightness * (config_.warm_mireds - config_.cool_mireds);
    const auto   mireds     = static_cast<uint16_t>(std::lround(
        sun_mireds + (config_.cool_mireds - sun_mireds) * (config_.cloud_cool_percent / 100.0) * overcast));

    LightState state{.on = true, .level = level, .mireds = mireds};

    // The amber glow follows the sun: bulbs further away see it as if it
    // were up to amber_sweep_deg higher, so they turn amber later and less.
    const double amber_elevation = pos.elevation_deg + config_.amber_sweep_deg * away;
    if (config_.amber_enabled && config_.golden_elevation_deg > 0.0 &&
        amber_elevation < config_.golden_elevation_deg) {
        // 0 at golden_elevation_deg, 1 at (or below) the horizon.
        const double golden =
            std::fmin(1.0, (config_.golden_elevation_deg - amber_elevation) / config_.golden_elevation_deg) *
            (1.0 - overcast); // no glow under a grey sky
        const double amber_mireds = 1e6 / config_.amber_kelvin;
        const double m            = mireds + golden * (amber_mireds - mireds);
        state.mireds              = static_cast<uint16_t>(std::lround(m));
        state.color               = ev::ColorMode::Xy;
        planckianXy(1e6 / m, state.color_x, state.color_y);
    }
    return state;
}

LightState DaylightSchedule::workState(const ev::TimeReading &time) const noexcept
{
    const double gate = activeGate(time);
    if (gate <= 0.0) {
        return {.on = false, .level = 0, .mireds = config_.work_warm_mireds};
    }
    uint8_t level = static_cast<uint8_t>(std::lround(gate * config_.max_level));
    if (level < config_.min_level) {
        level = config_.min_level;
    }

    // How far through the work hours, 0 to 1.
    const double progress = std::clamp(
        std::chrono::duration<double>{time.sinceMidnight() - config_.work.start} / (config_.work.end - config_.work.start),
        0.0, 1.0);
    const double warmth   = progress * progress; // ease-in: stays cool through the morning
    const auto   mireds   = static_cast<uint16_t>(
        std::lround(config_.work_cool_mireds + warmth * (config_.work_warm_mireds - config_.work_cool_mireds)));

    return {.on = true, .level = level, .mireds = mireds};
}

} // namespace sunsim::core
