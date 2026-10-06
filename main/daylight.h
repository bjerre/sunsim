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
// at the window's midpoint ("solar noon"); a cool-to-warm work light that
// tracks the work day rather than the sun (bright and cool in the morning,
// warm by evening). Both only run inside the active window
// (work days and hours, see Config). See solar.h for the
// sunrise/sunset calculation and DESIGN.md for the full math.
class DaylightSchedule {
public:
    static constexpr uint8_t kMondayToFriday = 0b0111110;
    static constexpr uint8_t kEveryDay       = 0b1111111;

    struct Config {
        double   latitude_deg  = 0.0; // degrees North (negative = South); 0 = equator
        double   longitude_deg = 0.0; // degrees East (negative = West); 0 = Greenwich
        uint16_t warm_mireds   = 370; // ~2700K
        uint16_t cool_mireds   = 153; // ~6500K
        uint8_t  min_level     = 1;
        uint8_t  max_level     = 254;

        // Active window, shared by all bulbs: lights are on only on
        // active_days between active_start_hour and active_end_hour, off
        // otherwise. Inside it the wall bulbs follow the sun curve unchanged
        // and the work bulb holds its fixed focus light; only the
        // fade_minutes at each edge scale brightness up from / down to off.
        double   active_start_hour = 8.0;
        double   active_end_hour   = 19.0;
        double   fade_minutes      = 10.0;
        uint8_t  active_days       = kMondayToFriday; // bit n = tm_wday n (0 = Sunday)

        // Work light runs cool -> warm across the active window: cool,
        // alerting light in the morning, warming steeply through the
        // afternoon to cut melatonin-suppressing blue light in the evening.
        // Ease-in (progress squared), so it stays near cool for most of the
        // morning. Interpolated in mireds (perceptually closer to even).
        uint16_t work_cool_mireds = 167; // ~6000K at active_start_hour
        uint16_t work_warm_mireds = 333; // ~3000K at active_end_hour
    };

    constexpr DaylightSchedule() noexcept : config_{} {}
    explicit constexpr DaylightSchedule(Config config) noexcept : config_(config) {}

    // utc_offset_hours: the local civil UTC offset in effect on local_time's
    // date, including DST, positive = East of Greenwich. Needed because
    // std::tm alone doesn't carry it (no portable tm_gmtoff on this
    // toolchain) - see LightRig::utcOffsetHours() for how it's derived.
    [[nodiscard]] LightState wallState(const std::tm &local_time, double utc_offset_hours) const noexcept;
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

} // namespace sunsim
