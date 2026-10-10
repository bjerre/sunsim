#pragma once

// Value types shared by every layer. Plain data only: no behaviour beyond
// trivial helpers, no ESP-IDF includes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace sunsim::ev {

// ---- Roles ----------------------------------------------------------------

// Which job a paired device does. Assigned at pairing time (console `pair`,
// or the build-time role manifest), never by join order.
enum class Role : uint8_t {
    WallA  = 0,
    WallB  = 1,
    WallC  = 2,
    Work   = 3,
    Button = 4,
};

inline constexpr std::size_t kRoleCount = 5;
inline constexpr std::size_t kBulbCount = 4; // WallA..Work; the Button is last

inline constexpr std::array<std::string_view, kRoleCount> kRoleNames = {"WallA", "WallB", "WallC", "Work", "Button"};

[[nodiscard]] constexpr std::size_t indexOf(Role role) noexcept { return static_cast<std::size_t>(role); }
[[nodiscard]] constexpr bool isBulb(Role role) noexcept { return role != Role::Button; }

[[nodiscard]] constexpr std::string_view roleName(std::size_t index) noexcept
{
    return index < kRoleCount ? kRoleNames[index] : std::string_view{"?"};
}
[[nodiscard]] constexpr std::string_view roleName(Role role) noexcept { return roleName(indexOf(role)); }

// Case-sensitive match against kRoleNames, e.g. "WallA".
[[nodiscard]] constexpr std::optional<Role> parseRole(std::string_view name) noexcept
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (kRoleNames[i] == name) {
            return static_cast<Role>(i);
        }
    }
    return std::nullopt;
}

// A paired device's addresses. short_addr is network-assigned and refreshed
// on every announce; endpoint is 0 until discovery finds it.
struct DeviceRecord {
    bool     valid      = false;
    uint64_t ext_addr   = 0;
    uint16_t short_addr = 0;
    uint8_t  endpoint   = 0;
};
using DeviceRecords = std::array<DeviceRecord, kRoleCount>;

// ---- Light state ----------------------------------------------------------

// How a LightState's colour is given.
enum class ColorMode : uint8_t {
    Temperature, // `mireds`
    Xy,          // CIE `color_x`/`color_y`: golden hour, warmer than colour-temperature mode can show
    HueSat,      // `hue`/`saturation`: saturated colours (red alert)
};

struct LightState {
    bool      on     = false;
    uint8_t   level  = 0; // 0-254; this and the colour are meaningful only if `on`
    ColorMode color  = ColorMode::Temperature;
    uint16_t  mireds = 0; // Temperature; for Xy, the blackbody temperature it sits at
    float     color_x    = 0.0f;
    float     color_y    = 0.0f;
    uint8_t   hue        = 0;
    uint8_t   saturation = 0;
};

// ---- Modes and links ------------------------------------------------------

// Which lighting program is running (core::ProgramHost runs it).
enum class Mode : uint8_t { Daylight, Off, RedAlert };

[[nodiscard]] constexpr const char *modeName(Mode mode) noexcept
{
    switch (mode) {
    case Mode::Daylight:
        return "daylight";
    case Mode::Off:
        return "off";
    case Mode::RedAlert:
        return "RED ALERT";
    }
    return "?";
}

enum class DeviceLink : uint8_t {
    Unpaired, // no device holds this role
    Silent,   // paired, but not heard from recently (or never, for a button)
    Online,   // heard from recently
};

struct DeviceStatus {
    DeviceLink link          = DeviceLink::Unpaired; // what status shows, from the registry's liveness
    bool       paired        = false;
    bool       has_endpoint  = false;
    int64_t    last_heard_us = 0;    // uptime; 0 = not since boot
    int8_t     rssi          = 0;    // of the last ack, if any
    bool       reachable     = true; // false: not answering, light commands paused
};

// ---- Program settings -----------------------------------------------------

inline constexpr uint8_t kMondayToFriday = 0b0111110; // bit n = tm_wday n (0 = Sunday)
inline constexpr uint8_t kEveryDay       = 0b1111111;

// Everything the daylight program can be tuned with. See DESIGN.md §4.
struct ProgramConfig {
    double   latitude_deg  = 0.0; // degrees North (negative = South); 0 = equator
    double   longitude_deg = 0.0; // degrees East (negative = West); 0 = Greenwich
    uint16_t warm_mireds   = 370; // ~2700K
    uint16_t cool_mireds   = 153; // ~6500K
    uint8_t  min_level     = 1;
    uint8_t  max_level     = 254;

    // Sun direction along the wall: the wall bulbs sit east-to-west on a
    // south wall; the one nearest the sun's east-west position gets the full
    // curve, those further along dim by up to this much (0 = off).
    uint8_t sun_shift_percent = 25;

    // Amber sunrise/sunset: below golden_elevation_deg of sun elevation the
    // wall colour slides along the blackbody line down to amber_kelvin at the
    // horizon (beyond most bulbs' ~2700K colour-temperature floor, so xy).
    bool   amber_enabled        = true;
    double golden_elevation_deg = 6.0;
    double amber_kelvin         = 1800.0;

    // Active window, shared by all bulbs: on only on active_days between
    // active_start_hour and active_end_hour; fade_minutes at each edge.
    double  active_start_hour = 8.0;
    double  active_end_hour   = 19.0;
    double  fade_minutes      = 10.0;
    uint8_t active_days       = kMondayToFriday;

    // Work light: cool -> warm (ease-in) across the core work hours, cool
    // before them and warm after.
    double   work_start_hour  = 8.5;  // 08:30
    double   work_end_hour    = 17.5; // 17:30
    uint16_t work_cool_mireds = 167;  // ~6000K
    uint16_t work_warm_mireds = 333;  // ~3000K
};

// Per-light peak intensity, 1-100 (% of full brightness). Button unused.
using Peaks = std::array<uint8_t, kRoleCount>;
inline constexpr Peaks kDefaultPeaks = {100, 100, 100, 90, 100};

// ---- Time -----------------------------------------------------------------

// What time the program runs on: the real clock, or the debug fast-day clock.
struct TimeReading {
    std::tm local{};
    double  utc_offset_hours = 0.0; // local civil offset incl. DST, + = East
    bool    valid            = false; // false until the clock has been set (SNTP)
    bool    simulated        = false; // debug fast-day clock
};

// Today's daylight as shown by the status display/console.
struct DayInfo {
    double sunrise_hour     = 0.0; // today, local fractional hours
    double sunset_hour      = 0.0;
    double day_length_hours = 0.0;
    bool   next_is_sunrise  = true;  // false = next event is today's sunset
    double next_event_hour  = 0.0;   // local hours of the next event
    double hours_until_next = 0.0;
};

// ---- Status ---------------------------------------------------------------

// Everything the `status`/`roles`/`config` console commands and the display
// show, gathered in one place so they always agree.
struct StatusSnapshot {
    int64_t                               uptime_us = 0;
    TimeReading                           time{};
    DayInfo                               day{};
    Mode                                  mode             = Mode::Daylight;
    bool                                  in_active_window = false;
    std::array<DeviceStatus, kRoleCount>  devices{};
    DeviceRecords                         records{};
    std::optional<Role>                   armed;
    ProgramConfig                         config{};
    Peaks                                 peaks = kDefaultPeaks;
};

} // namespace sunsim::ev
