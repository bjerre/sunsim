export module sunsim.core:day_info;

import std.compat;
import sunsim.ev;
import :solar;

export namespace sunsim::core {

// Day length and the next sunrise/sunset, for the status display/console.
// Pure, like :daylight and :solar.

[[nodiscard]] ev::DayInfo computeDayInfo(const ev::TimeReading &time, double latitude_deg, double longitude_deg) noexcept;

} // namespace sunsim::core

namespace sunsim::core {

ev::DayInfo computeDayInfo(const ev::TimeReading &time, double latitude_deg, double longitude_deg) noexcept
{
    using ev::Hours;
    constexpr Hours kDay{24.0};

    const Hours    now{time.sinceMidnight()};
    const Hours    offset{time.utc_offset};
    const int      day_of_year = time.dayOfYear();
    const SunTimes today       = computeSunTimes(latitude_deg, longitude_deg, day_of_year, offset);

    Hours day_length = today.sunset - today.sunrise;
    if (day_length < Hours{0}) {
        day_length += kDay; // window spills past midnight (near-polar summer)
    }

    ev::DayInfo info{
        .sunrise         = today.sunrise,
        .sunset          = today.sunset,
        .day_length      = day_length,
        .next_is_sunrise = true,
        .next_event      = today.sunrise,
        .until_next      = today.sunrise - now,
    };

    if (now < today.sunrise) {
        return info;
    }
    if (now < today.sunset) {
        info.next_is_sunrise = false;
        info.next_event      = today.sunset;
        info.until_next      = today.sunset - now;
        return info;
    }

    // After sunset: count to tomorrow's sunrise. Day 366 wraps to 1; the
    // sunrise difference across New Year is a fraction of a minute.
    const int      tomorrow = day_of_year >= 366 ? 1 : day_of_year + 1;
    const SunTimes next     = computeSunTimes(latitude_deg, longitude_deg, tomorrow, offset);
    info.next_event         = next.sunrise;
    info.until_next         = kDay - now + next.sunrise;
    return info;
}

} // namespace sunsim::core
