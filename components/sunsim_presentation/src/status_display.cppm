// A platform-neutral view of the status LCD. Renderers choose their own
// layout and graphics; they share the same text, tones, groups and links.

export module sunsim.presentation;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::presentation {

enum class Tone : uint8_t { Muted, Daylight, Tuning, Advisory, Watch, Warning, Sky };

struct DeviceGroupView {
    ev::Group       group;
    std::string_view name;
    char            label;
    uint8_t         count;
};

[[nodiscard]] constexpr char groupLabel(ev::Group group) noexcept
{
    switch (group) {
    case ev::Group::Wall:
        return 'D';
    case ev::Group::Work:
        return 'W';
    case ev::Group::Desk:
        return 'L';
    case ev::Group::Grow:
        return 'G';
    case ev::Group::Button:
        return 'B';
    }
    return '?';
}

inline constexpr auto kDeviceGroups = [] {
    std::array<DeviceGroupView, ev::kLayout.size()> groups{};
    for (std::size_t i = 0; i < ev::kLayout.size(); ++i) {
        const ev::GroupSpec &group = ev::kLayout[i];
        groups[i] = {group.group, group.name, groupLabel(group.group), group.count};
    }
    return groups;
}();

struct StatusDisplayData {
    std::string time        = "--:--";
    std::string weekday;
    std::string day_length  = "Waiting for";
    std::string next_event  = "time sync...";
    std::string sunrise     = "Rise --:--";
    std::string sunset      = "Set --:--";
    bool        time_valid  = false;
    double      sun_azimuth_deg   = 180.0;
    double      sun_elevation_deg = -90.0;
    std::string mode;
    Tone        mode_tone   = Tone::Muted;
    std::string sky;
    Tone        sky_tone    = Tone::Sky;
    std::optional<uint8_t> cloud_percent;
    ev::Precip precipitation = ev::Precip::None;
    std::array<DeviceGroupView, ev::kLayout.size()> groups = kDeviceGroups;
    ev::PerRole<ev::DeviceLink> device_links{};
};

struct ProgramFlowMemberData {
    ev::Role        role{};
    std::string_view behavior;
};

struct ProgramFlowModeData {
    ev::Mode mode = ev::Mode::Daylight;
    std::optional<ev::Press> button_press{};
};

struct ProgramFlowZoneData {
    std::string_view name;
    ev::Mode         mode = ev::Mode::Daylight;
    bool             gate_open = false;
    bool             gate_applies = true;
    std::array<ProgramFlowModeData, 4> available_modes{};
    std::size_t      mode_count = 0;
    std::array<ProgramFlowMemberData, ev::kLightCount> members{};
    std::size_t      member_count = 0;
};

struct ProgramFlowDisplayData {
    std::array<ProgramFlowZoneData, ev::kMaxZones> zones{};
    std::size_t zone_count = 0;
};

// Pure presentation transform: every renderer gets the same screen data.
[[nodiscard]] StatusDisplayData makeStatusDisplayData(const ev::StatusSnapshot &status);
[[nodiscard]] ProgramFlowDisplayData makeProgramFlowDisplayData(
    const ev::StatusSnapshot &status, const ev::Installation &installation = ev::kDefaultInstallation);

// Time-formatting helpers also used by the console.
[[nodiscard]] std::string clockText(ev::Hours after_midnight);
[[nodiscard]] std::string durationText(ev::Hours duration);

} // namespace sunsim::presentation

namespace sunsim::presentation {

std::string clockText(ev::Hours after_midnight)
{
    auto t = std::chrono::round<std::chrono::minutes>(after_midnight);
    if (t > std::chrono::days{1}) {
        t -= std::chrono::days{1};
    }
    return std::format("{:02}:{:02}", std::chrono::floor<std::chrono::hours>(t).count(),
                       (t % std::chrono::hours{1}).count());
}

std::string durationText(ev::Hours duration)
{
    const auto t = std::chrono::round<std::chrono::minutes>(duration);
    return std::format("{}h {:02}m", std::chrono::floor<std::chrono::hours>(t).count(),
                       (t % std::chrono::hours{1}).count());
}

namespace {

std::string_view behaviorName(const ev::Look &look)
{
    return std::visit(
        []<class T>(const T &) -> std::string_view {
            if constexpr (std::same_as<T, ev::SunCurve>) {
                return "sun curve";
            } else if constexpr (std::same_as<T, ev::WorkCurve>) {
                return "work curve";
            } else if constexpr (std::same_as<T, ev::OnOffOnly>) {
                return "on/off";
            } else {
                static_assert(std::same_as<T, void>, "Add a display label for this light behavior");
            }
        },
        look);
}

} // namespace

StatusDisplayData makeStatusDisplayData(const ev::StatusSnapshot &status)
{
    StatusDisplayData data;

    if (status.time.valid) {
        const ev::DayInfo &day = status.day;
        data.time_valid         = true;
        data.time               = std::format("{:%H:%M}", status.time.local);
        data.weekday            = std::format("{}{:%a}", status.time.simulated ? "sim " : "", status.time.weekday());
        data.day_length         = "Day " + durationText(day.day_length);
        data.next_event         = std::format("{} {}  {}", day.next_is_sunrise ? "Rise" : "Set",
                                              durationText(day.until_next), clockText(day.next_event));
        data.sunrise            = "Rise " + clockText(day.sunrise);
        data.sunset             = "Set " + clockText(day.sunset);
        data.sun_azimuth_deg    = day.sun_azimuth_deg;
        data.sun_elevation_deg  = day.sun_elevation_deg;
    }

    data.cloud_percent = status.sky.cloud_percent;
    if (status.sky.conditions) {
        data.precipitation = status.sky.conditions->precip;
    }

    const ev::ZoneStatus &zone = status.zones[ev::kZoneIds[0]];
    switch (zone.mode) {
    case ev::Mode::Daylight:
        data.mode      = zone.gate_open ? "daylight" : "idle";
        data.mode_tone = zone.gate_open ? Tone::Daylight : Tone::Muted;
        break;
    case ev::Mode::Off:
        data.mode      = "off";
        data.mode_tone = Tone::Muted;
        break;
    case ev::Mode::RedAlert:
        data.mode      = "RED ALERT";
        data.mode_tone = Tone::Warning;
        break;
    case ev::Mode::Tuning:
        data.mode      = "tuning";
        data.mode_tone = Tone::Tuning;
        break;
    }

    if (const ev::AlertLevel level = status.sky.alert(); level != ev::AlertLevel::None) {
        data.sky = ev::alertLevelName(level);
        switch (level) {
        case ev::AlertLevel::None:
            break;
        case ev::AlertLevel::Advisory:
            data.sky_tone = Tone::Advisory;
            break;
        case ev::AlertLevel::Watch:
            data.sky_tone = Tone::Watch;
            break;
        case ev::AlertLevel::Warning:
            data.sky_tone = Tone::Warning;
            break;
        }
    } else if (status.sky.conditions && status.sky.conditions->precip != ev::Precip::None) {
        data.sky = ev::precipName(status.sky.conditions->precip);
    } else if (status.sky.cloud_percent) {
        data.sky = std::format("cloud {}%", *status.sky.cloud_percent);
    }

    for (const ev::Role role : ev::kRoles) {
        data.device_links[role] = status.devices[role].link;
    }
    return data;
}

ProgramFlowDisplayData makeProgramFlowDisplayData(const ev::StatusSnapshot &status,
                                                  const ev::Installation &installation)
{
    ProgramFlowDisplayData data;
    for (const ev::ZoneId zone : installation.zoneIds()) {
        ProgramFlowZoneData &view = data.zones[data.zone_count++];
        view.name                   = installation.spec(zone).name;
        view.mode                   = status.zones[zone].mode;
        view.gate_open              = status.zones[zone].gate_open;
        view.gate_applies           = view.mode == ev::Mode::Daylight;
        for (const ev::Mode mode : installation.spec(zone).modes) {
            ProgramFlowModeData &choice = view.available_modes[view.mode_count++];
            choice.mode = mode;
            for (const ev::Controls &control : installation.spec(zone).controls) {
                if (control.single == mode) {
                    choice.button_press = ev::Press::Single;
                    break;
                }
                if (control.twice == mode) {
                    choice.button_press = ev::Press::Double;
                    break;
                }
                if (control.hold == mode) {
                    choice.button_press = ev::Press::Hold;
                    break;
                }
            }
        }
        for (const ev::Role role : ev::lights()) {
            if (installation.topology.zone_of[role] == zone) {
                view.members[view.member_count++] = {role, behaviorName(installation.topology.look_of[role])};
            }
        }
    }
    return data;
}

} // namespace sunsim::presentation
