#pragma once

#include "daylight.h"

namespace sunsim {

// Persists the user-tunable parts of DaylightSchedule::Config (active
// window hours, fade, work days) in NVS, set from the `config` console
// command. Each setting is its own key, so ones never saved - or added in a
// later firmware - keep the built-in default from Config.

// Overwrites the fields of `config` that have a saved value; leaves the rest.
void loadScheduleSettings(DaylightSchedule::Config &config);
void saveScheduleSettings(const DaylightSchedule::Config &config);

} // namespace sunsim
