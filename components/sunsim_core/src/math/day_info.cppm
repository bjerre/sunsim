module;

#include "sunsim/ev/types.h"

export module sunsim.core:day_info;

import std;

export namespace sunsim::core {

// Day length and the next sunrise/sunset, for the status display/console.
// Pure, like daylight.h and solar.h.

// utc_offset_hours: as for DaylightSchedule::wallState.
[[nodiscard]] ev::DayInfo computeDayInfo(const std::tm &local_time, double utc_offset_hours, double latitude_deg,
                                     double longitude_deg) noexcept;

} // namespace sunsim::core
