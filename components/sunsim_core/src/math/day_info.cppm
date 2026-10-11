export module sunsim.core:day_info;

import std.compat;
import sunsim.ev;
import :solar;

export namespace sunsim::core {

// Day length and the next sunrise/sunset, for the status display/console.
// Pure, like :daylight and :solar.

// utc_offset_hours: as for DaylightSchedule::wallState.
[[nodiscard]] ev::DayInfo computeDayInfo(const std::tm &local_time, double utc_offset_hours, double latitude_deg,
                                     double longitude_deg) noexcept;

} // namespace sunsim::core

namespace sunsim::core {

ev::DayInfo computeDayInfo(const std::tm &local_time, double utc_offset_hours, double latitude_deg,
                       double longitude_deg) noexcept
{
    const double hour        = local_time.tm_hour + local_time.tm_min / 60.0 + local_time.tm_sec / 3600.0;
    const int    day_of_year = local_time.tm_yday + 1;
    const SunTimes today     = computeSunTimes(latitude_deg, longitude_deg, day_of_year, utc_offset_hours);

    double day_length = today.sunset_hour - today.sunrise_hour;
    if (day_length < 0.0) {
        day_length += 24.0; // window spills past midnight (near-polar summer)
    }

    ev::DayInfo info{
        .sunrise_hour     = today.sunrise_hour,
        .sunset_hour      = today.sunset_hour,
        .day_length_hours = day_length,
        .next_is_sunrise  = true,
        .next_event_hour  = today.sunrise_hour,
        .hours_until_next = today.sunrise_hour - hour,
    };

    if (hour < today.sunrise_hour) {
        return info;
    }
    if (hour < today.sunset_hour) {
        info.next_is_sunrise  = false;
        info.next_event_hour  = today.sunset_hour;
        info.hours_until_next = today.sunset_hour - hour;
        return info;
    }

    // After sunset: count to tomorrow's sunrise. Day 366 wraps to 1; the
    // sunrise difference across New Year is a fraction of a minute.
    const int      tomorrow = day_of_year >= 366 ? 1 : day_of_year + 1;
    const SunTimes next     = computeSunTimes(latitude_deg, longitude_deg, tomorrow, utc_offset_hours);
    info.next_event_hour    = next.sunrise_hour;
    info.hours_until_next   = 24.0 - hour + next.sunrise_hour;
    return info;
}

} // namespace sunsim::core
