#pragma once

#include <array>
#include <cstdint>

#include "daylight.h"
#include "roles_store.h"

namespace sunsim {

// Persists the user-tunable parts of DaylightSchedule::Config (active
// window hours, fade, work days) in NVS, set from the `config` console
// command. Each setting is its own key, so ones never saved - or added in a
// later firmware - keep the built-in default from Config.

// Overwrites the fields of `config` that have a saved value; leaves the rest.
void loadScheduleSettings(DaylightSchedule::Config &config);
void saveScheduleSettings(const DaylightSchedule::Config &config);

// Per-light peak intensity, 1-100 (% of the bulb's full brightness), keyed
// by role name ("WallA.peak") so it stays with the light's place in the
// scene across unpair/re-pair. Roles never set keep their entry in `peaks`.
void loadPeakPercents(std::array<uint8_t, kRoleCount> &peaks);
void savePeakPercent(Role role, uint8_t percent);

} // namespace sunsim
