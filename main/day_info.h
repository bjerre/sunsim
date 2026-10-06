#pragma once

#include <ctime>

namespace sunsim {

// What the status display/console shows about today's daylight: how long
// the day is and how far away the next sunrise or sunset is. Pure, no
// Zigbee/hardware dependency, like daylight.h and solar.h.
struct DayInfo {
    double sunrise_hour;      // today, local fractional hours
    double sunset_hour;       // today, local fractional hours
    double day_length_hours;  // sunset - sunrise
    bool   next_is_sunrise;   // false = next event is today's sunset
    double next_event_hour;   // local fractional hours of the next event (tomorrow's sunrise after sunset)
    double hours_until_next;  // from local_time to that event
};

// utc_offset_hours: as for DaylightSchedule::wallState.
[[nodiscard]] DayInfo computeDayInfo(const std::tm &local_time, double utc_offset_hours, double latitude_deg,
                                     double longitude_deg) noexcept;

} // namespace sunsim
