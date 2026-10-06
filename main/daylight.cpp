#include "daylight.h"

#include <cmath>

#include "solar.h"

namespace sunsim {

namespace {
double hourOfDay(const std::tm &t) noexcept
{
    return t.tm_hour + t.tm_min / 60.0 + t.tm_sec / 3600.0;
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

LightState DaylightSchedule::wallState(const std::tm &local_time, double utc_offset_hours) const noexcept
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

    uint8_t level = static_cast<uint8_t>(std::lround(gate * brightness * config_.max_level));
    if (level < config_.min_level) {
        level = config_.min_level;
    }

    const uint16_t mireds = static_cast<uint16_t>(
        std::lround(config_.warm_mireds - brightness * (config_.warm_mireds - config_.cool_mireds)));

    return {.on = true, .level = level, .mireds = mireds};
}

LightState DaylightSchedule::workState(const std::tm &local_time) const noexcept
{
    const double gate = activeGate(local_time);
    if (gate <= 0.0) {
        return {.on = false, .level = 0, .mireds = config_.work_mireds};
    }
    uint8_t level = static_cast<uint8_t>(std::lround(gate * config_.work_level));
    if (level < config_.min_level) {
        level = config_.min_level;
    }
    return {.on = true, .level = level, .mireds = config_.work_mireds};
}

} // namespace sunsim
