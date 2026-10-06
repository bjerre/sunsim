#pragma once

#include <cstdint>
#include <ctime>

namespace sunsim {

struct LightState {
    bool     on     = false;
    uint8_t  level  = 0;  // 0-254, meaningful only if `on`
    uint16_t mireds = 0;  // color temperature, meaningful only if `on`
};

// Astronomical daylight model for the wall bulbs: warm and dim at the real
// sunrise/sunset for a configured location, brightest and coolest (bluest)
// at the window's midpoint ("solar noon"); a fixed cool-neutral "focus"
// spectrum during work hours for the work bulb, independent of the sun
// since it tracks a work schedule rather than daylight. See solar.h for the
// sunrise/sunset calculation and DESIGN.md for the full math.
class DaylightSchedule {
public:
    struct Config {
        double   latitude_deg  = 0.0; // degrees North (negative = South); 0 = equator
        double   longitude_deg = 0.0; // degrees East (negative = West); 0 = Greenwich
        uint16_t warm_mireds   = 370; // ~2700K
        uint16_t cool_mireds   = 153; // ~6500K
        uint8_t  min_level     = 1;
        uint8_t  max_level     = 254;

        double   work_start_hour = 8.0;
        double   work_end_hour   = 18.0;
        uint16_t work_mireds     = 200; // ~5000K, cool-neutral "focus" white
        uint8_t  work_level      = 230; // bright but not maxed
    };

    constexpr DaylightSchedule() noexcept : config_{} {}
    explicit constexpr DaylightSchedule(Config config) noexcept : config_(config) {}

    // utc_offset_hours: the local civil UTC offset in effect on local_time's
    // date, including DST, positive = East of Greenwich. Needed because
    // std::tm alone doesn't carry it (no portable tm_gmtoff on this
    // toolchain) - see LightRig::utcOffsetHours() for how it's derived.
    [[nodiscard]] LightState wallState(const std::tm &local_time, double utc_offset_hours) const noexcept;
    [[nodiscard]] LightState workState(const std::tm &local_time) const noexcept;

private:
    Config config_;
};

} // namespace sunsim
