// Value types shared by every layer. Plain data only: no behavior beyond
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

// Fractional hours: for the solar math and what's shown from it.
using Hours = std::chrono::duration<double, std::ratio<3600>>;

// A stretch of the day, from `start` to `end` after local midnight
// (start < end <= 24 h): the active hours, the work hours, the sync window.
struct DaySpan {
    std::chrono::minutes start{};
    std::chrono::minutes end{};

    [[nodiscard]] constexpr bool contains(std::chrono::seconds since_midnight) const noexcept
    {
        return since_midnight >= start && since_midnight < end;
    }
    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return start >= std::chrono::minutes{0} && end <= std::chrono::days{1} && start < end;
    }

    // Spelled out, like the colors': GCC 15 crashes compiling a file that
    // imports this and compares DaySpans with a defaulted ==.
    friend constexpr bool operator==(DaySpan a, DaySpan b) noexcept { return a.start == b.start && a.end == b.end; }
};

// A set of weekdays.
struct Weekdays {
    uint8_t bits = 0; // bit n: weekday n, as std::chrono::weekday::c_encoding() (0 = Sunday)

    [[nodiscard]] constexpr bool contains(std::chrono::weekday day) const noexcept
    {
        return (bits >> day.c_encoding()) & 1U;
    }
    constexpr void insert(std::chrono::weekday day) noexcept { bits |= static_cast<uint8_t>(1U << day.c_encoding()); }

    friend constexpr bool operator==(Weekdays, Weekdays) noexcept = default;
};
inline constexpr Weekdays kMondayToFriday{0b0111110};
inline constexpr Weekdays kEveryDay{0b1111111};

// ---- Roles ----------------------------------------------------------------

// What a device does. Each group can hold any number of devices.
enum class Group : uint8_t { Wall, Work, Button };

// What a device is, and so what it can do (caps()).
enum class Kind : uint8_t {
    ColorBulb,    // on/off, level, color temperature, xy and hue/saturation
    WhiteBulb,    // on/off, level, color temperature
    DimmableBulb, // on/off, level
    Plug,         // on/off, for whatever's plugged into it
    Button,       // presses: its On/Off client, bound to us
};

// What a device takes from us. A light takes at least On/Off; a button
// takes nothing (it sends).
struct Caps {
    bool power  = false; // On/Off
    bool level  = false; // Level Control
    bool mireds = false; // Color Control's color temperature
    bool color  = false; // Color Control's xy and hue/saturation
};

[[nodiscard]] constexpr Caps caps(Kind kind) noexcept
{
    switch (kind) {
    case Kind::ColorBulb:
        return {.power = true, .level = true, .mireds = true, .color = true};
    case Kind::WhiteBulb:
        return {.power = true, .level = true, .mireds = true};
    case Kind::DimmableBulb:
        return {.power = true, .level = true};
    case Kind::Plug:
        return {.power = true};
    case Kind::Button:
        break;
    }
    return {};
}

// N devices of one kind with one job.
struct GroupSpec {
    Group            group;
    std::string_view name;
    Kind             kind;
    uint8_t          count;
};

// The installation: what each group is and how many it has, in Group
// order. A group's members are lettered (WallA, WallB, ...); a group of one
// is just its name (Work, Button). Wall lights are lettered east to west:
// their order along the wall drives the sun-direction dimming.
inline constexpr std::array kLayout{
    GroupSpec{Group::Wall, "Wall", Kind::ColorBulb, 5},
    GroupSpec{Group::Work, "Work", Kind::ColorBulb, 1},
    GroupSpec{Group::Button, "Button", Kind::Button, 1},
};

// One device's job: its group, and which member of it. Assigned at pairing
// time (console `pair`, or its address in installation.cppm), never by join
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

[[nodiscard]] constexpr Kind kindOf(Role role) noexcept { return spec(role.group).kind; }
[[nodiscard]] constexpr Caps caps(Role role) noexcept { return caps(kindOf(role)); }

// A light: something we switch (a bulb or a plug), not a button.
[[nodiscard]] constexpr bool isLight(Role role) noexcept { return caps(role).power; }
[[nodiscard]] constexpr bool isButton(Role role) noexcept { return kindOf(role) == Kind::Button; }

// The lights (every role but the buttons), in slot order.
[[nodiscard]] constexpr auto lights() noexcept { return kRoles | std::views::filter(isLight); }
inline constexpr std::size_t kLightCount = std::ranges::count_if(kRoles, isLight);

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
static_assert(std::ranges::count_if(kRoles, isButton) <= 1,
              "one button at most: ZigbeeAdapter tells presses apart by its one address");

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

// Any number of roles, by slot: the weather lights.
using RoleSet = std::bitset<kRoleCount>;

[[nodiscard]] constexpr RoleSet roleSet(std::initializer_list<Role> roles) noexcept
{
    RoleSet set;
    for (const Role role : roles) {
        set.set(slot(role));
    }
    return set;
}

[[nodiscard]] constexpr bool contains(const RoleSet &set, Role role) noexcept { return set.test(slot(role)); }

// "none", or a comma list of what parseSelection() takes: "WallB,WallD",
// "wall", "WallA,Work".
[[nodiscard]] constexpr std::optional<RoleSet> parseRoleSet(std::string_view text) noexcept
{
    RoleSet set;
    if (detail::equalsIgnoringCase(text, "none")) {
        return set;
    }
    // (Not views::split: GCC 15 trips over its subrange across modules.)
    for (;;) {
        const auto comma     = text.find(',');
        const auto selection = parseSelection(text.substr(0, comma));
        if (!selection) {
            return std::nullopt;
        }
        for (const Role role : kRoles) {
            if (selects(*selection, role)) {
                set.set(slot(role));
            }
        }
        if (comma == std::string_view::npos) {
            return set;
        }
        text.remove_prefix(comma + 1);
    }
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

// A light's color, in whichever of the bulbs' three color modes shows it.

// A white on the blackbody line, by color temperature: mireds = 1e6 / kelvin.
struct Mireds {
    uint16_t value = 0;

    // Spelled out, as are Xy's and HueSat's: defaulted, GCC 15 crashes
    // compiling a file that imports this and compares them.
    friend constexpr bool operator==(Mireds a, Mireds b) noexcept { return a.value == b.value; }
};
// A CIE 1931 chromaticity: warmer than color-temperature mode can show
// (amber, below most bulbs' ~2700 K).
struct Xy {
    float x = 0.0f;
    float y = 0.0f;

    friend constexpr bool operator==(Xy a, Xy b) noexcept { return a.x == b.x && a.y == b.y; }
};
// A saturated color: Zigbee's hue (0-254 for 0-360 deg) and saturation (0-254).
struct HueSat {
    uint8_t hue        = 0;
    uint8_t saturation = 0;

    friend constexpr bool operator==(HueSat a, HueSat b) noexcept
    {
        return a.hue == b.hue && a.saturation == b.saturation;
    }
};
using Color = std::variant<Mireds, Xy, HueSat>;

inline constexpr uint8_t kMaxLevel = 254; // Zigbee's brightest

struct LightState {
    bool    on    = false;
    uint8_t level = 0; // 0-254; this and the color are meaningful only if `on`
    Color   color = Mireds{};
};

// ---- Modes and links ------------------------------------------------------

// Which lighting program is running (core::ProgramHost runs it).
enum class Mode : uint8_t { Daylight, Off, RedAlert, Tuning };
inline constexpr std::array<Mode, 4> kModes = {Mode::Daylight, Mode::Off, Mode::RedAlert, Mode::Tuning};

[[nodiscard]] constexpr const char *modeName(Mode mode) noexcept
{
    switch (mode) {
    case Mode::Daylight:
        return "daylight";
    case Mode::Off:
        return "off";
    case Mode::RedAlert:
        return "RED ALERT";
    case Mode::Tuning:
        return "tuning";
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
    // wall color slides along the blackbody line down to amber_kelvin at the
    // horizon (beyond most bulbs' ~2700K color-temperature floor, so xy).
    // The glow follows the sun along the wall: the bulb nearest it turns
    // amber first, and the far end lags as if the sun were up to
    // amber_sweep_deg higher (sunset: the west end deepest; 0 = all alike).
    bool    amber_enabled        = true;
    double  golden_elevation_deg = 6.0;
    double  amber_kelvin         = 1800.0;
    uint8_t amber_sweep_deg      = 3; // 0-kMaxAmberSweepDeg

    // Active hours: the office's gate (ev::kZones) lets its lights be on only
    // on active_days, in the active hours; fading in and out over `fade` at
    // each edge.
    DaySpan              active{std::chrono::hours{8}, std::chrono::hours{19}};
    std::chrono::minutes fade{10};
    Weekdays             active_days = kMondayToFriday;

    // Work light (ev::WorkCurve): cool -> warm (ease-in) across the core
    // work hours, cool before them and warm after.
    DaySpan  work{std::chrono::minutes{8 * 60 + 30}, std::chrono::minutes{17 * 60 + 30}};
    uint16_t work_cool_mireds = 167; // ~6000K
    uint16_t work_warm_mireds = 333; // ~3000K

    // Internet sync window: once a day in it, the clock is synced (and the
    // day's internet data fetched), retrying every 10 min within it on
    // failure. Before the active window, so the data is ready for the day.
    DaySpan sync{std::chrono::hours{4}, std::chrono::hours{5}};

    // Cloud cover over the wall (the hourly forecast, CloudCover): under
    // full overcast the wall is up to cloud_dim_percent dimmer and its
    // color up to cloud_cool_percent of the way to cool_mireds, and the
    // sun-direction dimming and amber glow fade out (diffuse light).
    bool    clouds_enabled     = true;
    uint8_t cloud_dim_percent  = 40;
    uint8_t cloud_cool_percent = 50;

    // Weather lights (core::Overlays): precip_lights pulse a color while
    // rain or snow falls, alert_lights yellow/orange/red for an NWS
    // advisory/watch/warning (an alert wins where they overlap). Off: every
    // light shows its program alone.
    bool    weather_enabled = true;
    RoleSet precip_lights   = roleSet({Role{Group::Wall, 2}});                      // WallC
    RoleSet alert_lights    = roleSet({Role{Group::Wall, 1}, Role{Group::Wall, 3}}); // WallB, WallD
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

// Per-light minimum level, 1-254 (raw Zigbee level): the lowest a light is
// sent while on, so it stays visibly lit down to sunset. Bulbs differ in
// where they go dark; find it with the console's `tune`.
using MinLevels = PerRole<uint8_t>;
inline constexpr MinLevels kDefaultMinLevels = [] {
    MinLevels levels{};
    for (const Role role : kRoles) {
        levels[role] = 1;
    }
    return levels;
}();

// ---- Weather --------------------------------------------------------------

// The hourly cloud cover forecast (InternetSync fetches it each sync): 48
// local hours from 00:00 of the day it was fetched.
struct CloudCover {
    static constexpr uint8_t     kNoData = 0xFF;
    static constexpr std::size_t kHours  = 48;

    std::optional<std::chrono::local_days> day; // hour 0's date; none: no forecast yet
    std::array<uint8_t, kHours>   percent = [] {
        std::array<uint8_t, kHours> a{};
        a.fill(kNoData);
        return a;
    }();
};

// What's falling now, from the WMO weather code, and how hard.
enum class Precip : uint8_t { None, Drizzle, Rain, FreezingRain, Snow, Thunderstorm };
enum class Intensity : uint8_t { Light, Moderate, Heavy };

[[nodiscard]] constexpr const char *precipName(Precip precip) noexcept
{
    switch (precip) {
    case Precip::None:
        return "none";
    case Precip::Drizzle:
        return "drizzle";
    case Precip::Rain:
        return "rain";
    case Precip::FreezingRain:
        return "freezing rain";
    case Precip::Snow:
        return "snow";
    case Precip::Thunderstorm:
        return "thunderstorm";
    }
    return "?";
}

[[nodiscard]] constexpr const char *intensityName(Intensity intensity) noexcept
{
    switch (intensity) {
    case Intensity::Light:
        return "light";
    case Intensity::Moderate:
        return "moderate";
    case Intensity::Heavy:
        return "heavy";
    }
    return "?";
}

// The weather now (Open-Meteo's `current`: 15-minute model data, the best
// estimate of now rather than an observation).
struct Conditions {
    uint8_t   cloud_percent = 0;
    Precip    precip        = Precip::None;
    Intensity intensity     = Intensity::Light;

    friend constexpr bool operator==(const Conditions &, const Conditions &) noexcept = default;
};

// NWS's alert tiers, as its event names give them: "Winter Storm Warning".
enum class AlertLevel : uint8_t { None, Advisory, Watch, Warning };

[[nodiscard]] constexpr const char *alertLevelName(AlertLevel level) noexcept
{
    switch (level) {
    case AlertLevel::None:
        return "none";
    case AlertLevel::Advisory:
        return "advisory";
    case AlertLevel::Watch:
        return "watch";
    case AlertLevel::Warning:
        return "warning";
    }
    return "?";
}

struct Alert {
    AlertLevel           level = AlertLevel::None;
    std::array<char, 32> event{}; // "Winter Storm Warning", NUL-terminated, cut to fit
    Instant              until{}; // when it ends

    friend constexpr bool operator==(const Alert &, const Alert &) noexcept = default;
};

// The NWS alerts for the location, highest level first.
struct Alerts {
    static constexpr std::size_t kMax = 4;

    std::array<Alert, kMax> items{};
    uint8_t                 count = 0;

    [[nodiscard]] std::span<const Alert> list() const noexcept { return {items.data(), count}; }

    friend constexpr bool operator==(const Alerts &, const Alerts &) noexcept = default;
};

// Live weather older than this (three missed 15-minute fetches) is ignored:
// cloud cover falls back to the forecast, the weather lights to their program.
inline constexpr std::chrono::minutes kWeatherFreshFor{45};

// The sky now, as core::SkyModel concludes it from the forecast and the live
// weather: what the daylight program, the weather lights and the status show.
struct Sky {
    std::optional<uint8_t>    cloud_percent;      // none: no data at all (clear assumed)
    bool                      cloud_live = false; // from the live conditions, not the forecast
    std::optional<Conditions> conditions;         // live, while fresh
    Alerts                    alerts{};           // in force, highest level first

    [[nodiscard]] constexpr AlertLevel alert() const noexcept
    {
        return alerts.count > 0 ? alerts.items[0].level : AlertLevel::None;
    }

    friend constexpr bool operator==(const Sky &, const Sky &) noexcept = default;
};

// ---- Time -----------------------------------------------------------------

// What time the program runs on: the real clock, or the debug fast-day clock.
struct TimeReading {
    std::chrono::local_seconds local{};      // the wall-clock time where the lights are
    std::chrono::minutes       utc_offset{}; // local - UTC, DST included (+ = East)
    bool                       valid     = false; // false until the clock has been set (SNTP)
    bool                       simulated = false; // debug fast-day clock

    [[nodiscard]] constexpr std::chrono::local_days day() const noexcept
    {
        return std::chrono::floor<std::chrono::days>(local);
    }
    [[nodiscard]] constexpr std::chrono::seconds sinceMidnight() const noexcept { return local - day(); }
    [[nodiscard]] constexpr std::chrono::weekday weekday() const noexcept { return std::chrono::weekday{day()}; }
    [[nodiscard]] constexpr int dayOfYear() const noexcept // 1-366
    {
        const std::chrono::year_month_day date{day()};
        return static_cast<int>((day() - std::chrono::local_days{date.year() / 1 / 1}).count()) + 1;
    }
};

// Today's daylight as shown by the status display/console: times are after
// local midnight.
struct DayInfo {
    Hours sunrise{};
    Hours sunset{};
    Hours day_length{};
    bool  next_is_sunrise = true; // false = next event is today's sunset
    Hours next_event{};           // when the next event is
    Hours until_next{};           // how long until it
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
