// Value types shared by every layer. Plain data only: no behaviour beyond
// trivial helpers, no ESP-IDF includes.

module;

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif

export module sunsim.ev:types;

import std.compat;

export namespace sunsim::ev {

// ---- Clock ----------------------------------------------------------------

// Steady time since boot (the esp_timer; steady_clock on the host). Only
// the platform layer calls now(): core code gets time from events, which
// keeps it testable.
struct BootClock {
    using rep                       = int64_t;
    using period                    = std::micro;
    using duration                  = std::chrono::duration<rep, period>;
    using time_point                = std::chrono::time_point<BootClock>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept;
};
using Instant = BootClock::time_point;

// Zigbee transition times: tenths of a second.
using Transition = std::chrono::duration<uint16_t, std::deci>;

// ---- Roles ----------------------------------------------------------------

// What a device does. Each group can hold any number of devices.
enum class Group : uint8_t { Wall, Work, Button };

struct GroupSpec {
    Group            group;
    std::string_view name;
    uint8_t          count;
};

// The installation: how many devices each group has, in Group order. A
// group's members are lettered (WallA, WallB, ...); a group of one is just
// its name (Work, Button). Wall lights are lettered east to west: their
// order along the wall drives the sun-direction dimming.
inline constexpr std::array kLayout{
    GroupSpec{Group::Wall, "Wall", 5},
    GroupSpec{Group::Work, "Work", 1},
    GroupSpec{Group::Button, "Button", 1},
};

// One device's job: its group, and which member of it. Assigned at pairing
// time (console `pair`, or the build-time role manifest), never by join
// order.
struct Role {
    Group   group = Group::Wall;
    uint8_t index = 0; // within the group; for the wall, 0 = the east end

    friend constexpr bool operator==(Role, Role) noexcept = default;
};

[[nodiscard]] constexpr const GroupSpec &spec(Group group) noexcept
{
    return kLayout[std::to_underlying(group)];
}

inline constexpr std::size_t kRoleCount =
    std::ranges::fold_left(kLayout | std::views::transform(&GroupSpec::count), 0uz, std::plus{});

// Where a role's data lives in a PerRole: groups in Group order, members in
// order within each.
[[nodiscard]] constexpr std::size_t slot(Role role) noexcept
{
    std::size_t s = role.index;
    for (std::size_t g = 0; g < std::to_underlying(role.group); ++g) {
        s += kLayout[g].count;
    }
    return s;
}

// Every role, in slot order.
inline constexpr std::array<Role, kRoleCount> kRoles = [] {
    std::array<Role, kRoleCount> roles{};
    for (const GroupSpec &g : kLayout) {
        for (uint8_t i = 0; i < g.count; ++i) {
            roles[slot({g.group, i})] = {g.group, i};
        }
    }
    return roles;
}();

// A group's roles, in order (for the wall: east to west).
[[nodiscard]] constexpr std::span<const Role> members(Group group) noexcept
{
    return std::span{kRoles}.subspan(slot({group, 0}), spec(group).count);
}

[[nodiscard]] constexpr bool isBulb(Role role) noexcept { return role.group != Group::Button; }

// The bulbs (every role but the buttons), in slot order.
[[nodiscard]] constexpr auto bulbs() noexcept { return kRoles | std::views::filter(isBulb); }
inline constexpr std::size_t kBulbCount = std::ranges::count_if(kRoles, isBulb);

// One T per role, indexed by the role.
template <class T> struct PerRole {
    std::array<T, kRoleCount> items{};

    [[nodiscard]] constexpr T       &operator[](Role role) noexcept { return items[slot(role)]; }
    [[nodiscard]] constexpr const T &operator[](Role role) const noexcept { return items[slot(role)]; }
};

namespace detail {
static_assert(std::ranges::all_of(kLayout, [](const GroupSpec &g) { return g.count >= 1 && g.count <= 26; }),
              "a group has 1-26 members (lettered A-Z)");
static_assert(std::ranges::equal(kLayout | std::views::transform(&GroupSpec::group),
                                 std::array{Group::Wall, Group::Work, Group::Button}),
              "kLayout must list the groups in Group order");

// A role's name, NUL-terminated (it's printed with %s).
struct RoleName {
    std::array<char, 12> chars{};
    std::size_t          size = 0;
};

inline constexpr PerRole<RoleName> kRoleNames = [] {
    PerRole<RoleName> names{};
    for (const Role role : kRoles) {
        RoleName       &n = names[role];
        const GroupSpec &g = spec(role.group);
        std::ranges::copy(g.name, n.chars.begin());
        n.size = g.name.size();
        if (g.count > 1) {
            n.chars[n.size++] = static_cast<char>('A' + role.index);
        }
    }
    return names;
}();
static_assert(std::ranges::all_of(kRoleNames.items, [](const RoleName &n) { return n.size < n.chars.size(); }));

[[nodiscard]] constexpr bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return std::ranges::equal(a, b, {}, lower, lower);
}
} // namespace detail

// "WallA" ... "WallE", "Work", "Button". NUL-terminated.
[[nodiscard]] constexpr std::string_view roleName(Role role) noexcept
{
    const detail::RoleName &n = detail::kRoleNames[role];
    return {n.chars.data(), n.size};
}

// A role by name, ignoring case ("WallA", "walla").
[[nodiscard]] constexpr std::optional<Role> parseRole(std::string_view name) noexcept
{
    for (const Role role : kRoles) {
        if (detail::equalsIgnoringCase(roleName(role), name)) {
            return role;
        }
    }
    return std::nullopt;
}

// Which roles a request is about: every role, one group, or one role.
struct AllRoles {
    friend constexpr bool operator==(AllRoles, AllRoles) noexcept = default;
};
using Selection = std::variant<AllRoles, Group, Role>;

[[nodiscard]] constexpr bool selects(const Selection &selection, Role role) noexcept
{
    if (const auto *group = std::get_if<Group>(&selection)) {
        return role.group == *group;
    }
    if (const auto *one = std::get_if<Role>(&selection)) {
        return role == *one;
    }
    return true;
}

// "all", a role ("WallC") or a group ("wall"), ignoring case. A group of
// one is its role ("Work").
[[nodiscard]] constexpr std::optional<Selection> parseSelection(std::string_view text) noexcept
{
    if (detail::equalsIgnoringCase(text, "all")) {
        return AllRoles{};
    }
    if (const auto role = parseRole(text)) {
        return *role;
    }
    for (const GroupSpec &g : kLayout) {
        if (detail::equalsIgnoringCase(g.name, text)) {
            return g.group;
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
using DeviceRecords = PerRole<DeviceRecord>;

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
inline constexpr std::array<Mode, 3> kModes = {Mode::Daylight, Mode::Off, Mode::RedAlert};

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
    DeviceLink             link         = DeviceLink::Unpaired; // what status shows, from the registry's liveness
    bool                   paired       = false;
    bool                   has_endpoint = false;
    std::optional<Instant> last_heard{};        // empty: not since boot
    int8_t                 rssi         = 0;    // of the last ack, if any
    bool                   reachable    = true; // false: not answering, light commands paused
};

// ---- Program settings -----------------------------------------------------

inline constexpr uint8_t kMondayToFriday = 0b0111110; // bit n = tm_wday n (0 = Sunday)
inline constexpr uint8_t kEveryDay       = 0b1111111;

// Everything the programs (and the internet sync) can be tuned with. See
// DESIGN.md §4.
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
    // The glow follows the sun along the wall: the bulb nearest it turns
    // amber first, and the far end lags as if the sun were up to
    // amber_sweep_deg higher (sunset: the west end deepest; 0 = all alike).
    bool    amber_enabled        = true;
    double  golden_elevation_deg = 6.0;
    double  amber_kelvin         = 1800.0;
    uint8_t amber_sweep_deg      = 3; // 0-kMaxAmberSweepDeg

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

    // Internet sync window: once a day in it, the clock is synced (and the
    // day's internet data fetched), retrying every 10 min within it on
    // failure. Before the active window, so the data is ready for the day.
    double sync_start_hour = 4.0;
    double sync_end_hour   = 5.0;
};

inline constexpr uint8_t kMaxAmberSweepDeg = 30;

// Per-light peak intensity, 1-100 (% of full brightness). Buttons unused.
using Peaks = PerRole<uint8_t>;
inline constexpr Peaks kDefaultPeaks = [] {
    Peaks peaks{};
    for (const Role role : kRoles) {
        peaks[role] = role.group == Group::Work ? 90 : 100;
    }
    return peaks;
}();

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
    Instant                               at{}; // when this snapshot was taken
    TimeReading                           time{};
    DayInfo                               day{};
    Mode                                  mode             = Mode::Daylight;
    bool                                  in_active_window = false;
    PerRole<DeviceStatus>                 devices{};
    DeviceRecords                         records{};
    std::optional<Role>                   armed;
    ProgramConfig                         config{};
    Peaks                                 peaks = kDefaultPeaks;
};

} // namespace sunsim::ev

namespace sunsim::ev {

BootClock::time_point BootClock::now() noexcept
{
#if defined(ESP_PLATFORM)
    return time_point{duration{esp_timer_get_time()}};
#else
    return time_point{std::chrono::duration_cast<duration>(std::chrono::steady_clock::now().time_since_epoch())};
#endif
}

} // namespace sunsim::ev
