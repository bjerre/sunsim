#include "sunsim/core/daylight.h"

#include <cmath>

#include "sunsim/core/solar.h"

namespace sunsim::core {

namespace {
double hourOfDay(const std::tm &t) noexcept
{
    return t.tm_hour + t.tm_min / 60.0 + t.tm_sec / 3600.0;
}

// CIE 1931 xy of a blackbody at `kelvin` (Kim et al. cubic spline
// approximation of the Planckian locus, valid 1667-25000K).
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
} // namespace

double DaylightSchedule::activeGate(const std::tm &local_time) const noexcept
{
    if (((config_.active_days >> local_time.tm_wday) & 1U) == 0) {
        return 0.0;
    }
    const double hour = hourOfDay(local_time);
    if (hour < config_.active_start_hour || hour >= config_.active_end_hour) {
        return 0.0;
    }
    if (config_.fade_minutes <= 0.0) {
        return 1.0;
    }
    const double fade_hours = config_.fade_minutes / 60.0;
    const double fade_in    = (hour - config_.active_start_hour) / fade_hours;
    const double fade_out   = (config_.active_end_hour - hour) / fade_hours;
    return std::fmin(1.0, std::fmin(fade_in, fade_out));
}

LightState DaylightSchedule::wallState(const std::tm &local_time, double utc_offset_hours,
                                       double wall_position) const noexcept
{
    const double gate = activeGate(local_time);
    if (gate <= 0.0) {
        return {.on = false, .level = 0, .mireds = config_.warm_mireds};
    }

    const double hour        = hourOfDay(local_time);
    const int    day_of_year = local_time.tm_yday + 1;
    const SunTimes sun =
        computeSunTimes(config_.latitude_deg, config_.longitude_deg, day_of_year, utc_offset_hours);

    double sunrise = sun.sunrise_hour;
    double sunset  = sun.sunset_hour;
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
    const double brightness   = std::sin(M_PI * day_progress); // 0 at edges, 1 at solar noon

    // Sun direction: sin(azimuth) is the sun's east-west position (+1 due
    // East, 0 due South, -1 due West); bulbs further along the wall from it
    // dim, by up to sun_shift_percent at the opposite end (distance 2).
    const SunPosition pos =
        computeSunPosition(config_.latitude_deg, config_.longitude_deg, day_of_year, hour, utc_offset_hours);
    const double sun_x     = std::sin(pos.azimuth_deg * M_PI / 180.0);
    const double direction = 1.0 - (config_.sun_shift_percent / 100.0) * std::fabs(wall_position - sun_x) / 2.0;

    uint8_t level = static_cast<uint8_t>(std::lround(gate * direction * brightness * config_.max_level));
    if (level < config_.min_level) {
        level = config_.min_level;
    }

    const uint16_t mireds = static_cast<uint16_t>(
        std::lround(config_.warm_mireds - brightness * (config_.warm_mireds - config_.cool_mireds)));

    LightState state{.on = true, .level = level, .mireds = mireds};

    if (config_.amber_enabled && config_.golden_elevation_deg > 0.0 &&
        pos.elevation_deg < config_.golden_elevation_deg) {
        // 0 at golden_elevation_deg, 1 at (or below) the horizon.
        const double golden = std::fmin(
            1.0, (config_.golden_elevation_deg - pos.elevation_deg) / config_.golden_elevation_deg);
        const double amber_mireds = 1e6 / config_.amber_kelvin;
        const double m            = mireds + golden * (amber_mireds - mireds);
        state.mireds              = static_cast<uint16_t>(std::lround(m));
        state.use_xy              = true;
        planckianXy(1e6 / m, state.color_x, state.color_y);
    }
    return state;
}

LightState DaylightSchedule::workState(const std::tm &local_time) const noexcept
{
    const double gate = activeGate(local_time);
    if (gate <= 0.0) {
        return {.on = false, .level = 0, .mireds = config_.work_warm_mireds};
    }
    uint8_t level = static_cast<uint8_t>(std::lround(gate * config_.max_level));
    if (level < config_.min_level) {
        level = config_.min_level;
    }

    const double window   = config_.work_end_hour - config_.work_start_hour;
    const double progress = std::fmin(1.0, std::fmax(0.0, (hourOfDay(local_time) - config_.work_start_hour) / window));
    const double warmth   = progress * progress; // ease-in: stays cool through the morning
    const auto   mireds   = static_cast<uint16_t>(
        std::lround(config_.work_cool_mireds + warmth * (config_.work_warm_mireds - config_.work_cool_mireds)));

    return {.on = true, .level = level, .mireds = mireds};
}

} // namespace sunsim::core
