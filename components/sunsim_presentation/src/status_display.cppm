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
    std::string mode;
    Tone        mode_tone   = Tone::Muted;
    std::string sky;
    Tone        sky_tone    = Tone::Sky;
    std::array<DeviceGroupView, ev::kLayout.size()> groups = kDeviceGroups;
    ev::PerRole<ev::DeviceLink> device_links{};
};

// Pure presentation transform: every renderer gets the same screen data.
[[nodiscard]] StatusDisplayData makeStatusDisplayData(const ev::StatusSnapshot &status);

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

StatusDisplayData makeStatusDisplayData(const ev::StatusSnapshot &status)
{
    StatusDisplayData data;

    if (status.time.valid) {
        const ev::DayInfo &day = status.day;
        data.time               = std::format("{:%H:%M}", status.time.local);
        data.weekday            = std::format("{}{:%a}", status.time.simulated ? "sim " : "", status.time.weekday());
        data.day_length         = "Day " + durationText(day.day_length);
        data.next_event         = std::format("{} {}  {}", day.next_is_sunrise ? "Rise" : "Set",
                                              durationText(day.until_next), clockText(day.next_event));
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

} // namespace sunsim::presentation
