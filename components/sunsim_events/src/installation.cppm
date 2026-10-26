// The room, as data: which device is which, and the zones: what each light
// shows, when it may be on, and what chooses its mode. Edit, build, flash.
// The words are :topology's.

export module sunsim.ev:installation;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
export import :layout;
import :topology;
import :types;

export namespace sunsim::ev {

// Each device's IEEE (EUI-64) address, from its label: when it joins, it
// takes that role. A role left at 0 is claimed with `pair <role>` instead.
// (`./sunsim monitor` logs an unknown device's address as it joins.)
inline constexpr List<Address, kRoleCount> kAddresses{
    {"WallA", 0x0000000000000000},
    {"WallB", 0x0000000000000000},
    {"WallC", 0x0000000000000000},
    {"WallD", 0x0000000000000000},
    {"WallE", 0x0000000000000000},
    {"Work", 0x0000000000000000},
    {"Desk", 0x0000000000000000},
    {"Grow", 0x0000000000000000},
    {"Button", 0x0000000000000000},
};

// Lights that share a mode and a gate. Every light is in exactly one.
inline constexpr std::array kZones{
    ZoneSpec{
        .name   = "office",
        .lights = {{.lights = "Wall", .look = SunCurve{}},
                   {.lights = "Work", .look = WorkCurve{}, .peak = 90}},
        // The active hours (`config hours`, `days`, `fade`).
        .gate     = {Window{.hours = &ProgramConfig::active, .days = &ProgramConfig::active_days,
                            .fade = &ProgramConfig::fade}},
        .controls = {{.by = "Button"}}, // single: daylight, double: red alert, hold: off
        .weather  = Weather{.precip = {"WallC"}, .alerts = {"WallB", "WallD"}},
    },
    ZoneSpec{
        .name   = "desk-downlight",
        .lights = {{.lights = "Desk", .look = OnOffOnly{}}},
        .gate   = {Window{.hours = DaySpan{std::chrono::hours{8}, std::chrono::hours{19}}}},
        .modes  = {Mode::Daylight},
    },
    ZoneSpec{
        .name   = "bookcase-grow-light",
        .lights = {{.lights = "Grow", .look = OnOffOnly{}}},
        .gate   = {Window{.hours = DaySpan{std::chrono::minutes{3 * 60 + 45},
                                           std::chrono::minutes{8 * 60 + 45}}}},
        .modes  = {Mode::Daylight},
    },
};

// ---- Derived: the tables above per role and per zone, checked as they compile ----

inline constexpr Addresses kAddressOf = addressesOf(kAddresses);

inline constexpr Topology                         kTopology      = topologyOf(kZones);
inline constexpr PerRole<std::optional<ZoneId>>   kZoneOf        = kTopology.zone_of;
inline constexpr PerRole<Look>                    kLookOf        = kTopology.look_of;
inline constexpr Peaks                            kDefaultPeaks  = kTopology.peak_of;
inline constexpr PerRole<std::optional<Controls>> kControlsOf    = kTopology.controls_of;
inline constexpr WeatherLights                    kWeatherLights = kTopology.weather;

inline constexpr std::size_t kZoneCount = kZones.size();
static_assert(kZoneCount >= 1 && kZoneCount <= 255, "1-255 zones");

[[nodiscard]] constexpr const ZoneSpec &spec(ZoneId zone) noexcept { return kZones[zone.index]; }

// Every zone, in order.
inline constexpr std::array<ZoneId, kZoneCount> kZoneIds = [] {
    std::array<ZoneId, kZoneCount> ids{};
    for (std::size_t i = 0; i < kZoneCount; ++i) {
        ids[i] = {static_cast<uint8_t>(i)};
    }
    return ids;
}();

// One T per zone, indexed by the zone.
template <class T> struct PerZone {
    std::array<T, kZoneCount> items{};

    [[nodiscard]] constexpr T       &operator[](ZoneId zone) noexcept { return items[zone.index]; }
    [[nodiscard]] constexpr const T &operator[](ZoneId zone) const noexcept { return items[zone.index]; }
};

} // namespace sunsim::ev
