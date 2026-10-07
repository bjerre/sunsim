#pragma once

#include <cstdint>
#include <ctime>

#include "sunsim/ev/types.h"

namespace sunsim::core {

using ev::LightState;

// Astronomical daylight model for the wall bulbs: warm and dim at the real
// sunrise/sunset for a configured location, brightest and coolest (bluest)
// at the window's midpoint ("solar noon"); a cool-to-warm work light that
// tracks the work day rather than the sun (bright and cool in the morning,
// warm by evening). Both only run inside the active window
// (work days and hours, see Config). See solar.h for the
// sunrise/sunset calculation and DESIGN.md for the full math.
class DaylightSchedule {
public:
    // All tunables live in ev::ProgramConfig (shared with settings/console).
    using Config = ev::ProgramConfig;

    constexpr DaylightSchedule() noexcept : config_{} {}
    explicit constexpr DaylightSchedule(Config config) noexcept : config_(config) {}

    // utc_offset_hours: the local civil UTC offset in effect on local_time's
    // date, including DST, positive = East of Greenwich. Needed because
    // std::tm alone doesn't carry it (no portable tm_gmtoff on this
    // toolchain) - see platform::Clock for how it's derived.
    // wall_position: where the bulb sits along the (south) wall, +1 = east
    // end, 0 = middle, -1 = west end; drives the sun-direction dimming.
    [[nodiscard]] LightState wallState(const std::tm &local_time, double utc_offset_hours,
                                       double wall_position = 0.0) const noexcept;
    [[nodiscard]] LightState workState(const std::tm &local_time) const noexcept;

    // True inside the active window (lights may be on), including its fades.
    [[nodiscard]] bool inActiveWindow(const std::tm &local_time) const noexcept
    {
        return activeGate(local_time) > 0.0;
    }

    [[nodiscard]] const Config &config() const noexcept { return config_; }
    void setConfig(const Config &config) noexcept { config_ = config; }

private:
    // 0 = off (outside active days/hours), 1 = full curve, ramping between
    // the two across the fade at each edge of the active window.
    [[nodiscard]] double activeGate(const std::tm &local_time) const noexcept;

    Config config_;
};

} // namespace sunsim::core
