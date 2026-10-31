export module sunsim.core:daylight_program;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :daylight;
import :gate;
import :program;

export namespace sunsim::core {

// The daylight program's ungarnished light target for one role and time.
// Shared with diagnostics and displays so they sample the same curve and gate.
[[nodiscard]] ev::LightState daylightTarget(ev::Role role, const ev::ProgramConfig &config,
                                            const ev::Peaks &peaks, const ev::MinLevels &min_levels,
                                            const ev::TimeReading &time, double cloud,
                                            const ev::Installation &installation = ev::kDefaultInstallation) noexcept;

// The normal program: each light shows its look (ev::kLookOf: the wall's
// sun curve, with the cloud cover live, else forecast; the work light's
// cool -> warm day) while its zone's gate (the active hours) lets it,
// across the light's range from its minimum level to its peak intensity.
// Every ProgramTick ramps over the update interval; refreshes and setting
// changes re-apply at once with a quick transition. Until the clock has been set (no SNTP yet) every bulb
// is held off.
class DaylightProgram final : public Program {
public:
    static constexpr ev::Mode kMode = ev::Mode::Daylight;
    using Program::Program;

    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now);
    void on(const ev::ProgramTick &e);
    void on(const ev::ConfigChanged &e);
    void on(const ev::SkyChanged &e) { cloud_percent_ = e.sky.cloud_percent.value_or(0); }

private:
    void emit(ev::Role role, const ev::TimeReading &time, ev::Transition transition);

    ev::ProgramConfig config_{};
    ev::Peaks         peaks_         = ev::kDefaultPeaks;
    ev::MinLevels     min_levels_    = ev::kDefaultMinLevels;
    uint8_t           cloud_percent_ = 0; // the sky now (none known: clear)
    bool              clock_warned_  = false;
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
constexpr const char *kTag = "PROGRAM";

// Fixed, short transition for changes that should feel immediate rather
// than mid-animation: resuming, a bulb (re)joining, a setting change.
constexpr ev::Transition kSnapTransition = std::chrono::seconds{2};

// Where a light sits in its group, evenly spaced in letter order: +1 the
// first, -1 the last (for the wall: east to west; see LookContext).
double positionInGroup(ev::Role role) noexcept
{
    const std::size_t count = ev::spec(role.group).count;
    return count < 2 ? 0.0 : 1.0 - 2.0 * role.index / static_cast<double>(count - 1);
}

uint8_t levelFor(ev::Role role, double brightness, const ev::Peaks &peaks,
                 const ev::MinLevels &min_levels) noexcept
{
    const double top   = std::max(1.0, ev::kMaxLevel * peaks[role] / 100.0);
    const double floor = std::min<double>(min_levels[role], top);
    return static_cast<uint8_t>(std::lround(floor + (top - floor) * std::clamp(brightness, 0.0, 1.0)));
}
} // namespace

ev::LightState daylightTarget(ev::Role role, const ev::ProgramConfig &config, const ev::Peaks &peaks,
                              const ev::MinLevels &min_levels, const ev::TimeReading &time, double cloud,
                              const ev::Installation &installation) noexcept
{
    const auto &topology = installation.topology;
    if (!time.valid || !topology.zone_of[role]) {
        return {};
    }
    const double open = openness(installation.spec(*topology.zone_of[role]).gate, time, config);
    const auto glow = show(topology.look_of[role], {.config = config,
                                                    .time = time,
                                                    .position = positionInGroup(role),
                                                    .cloud = cloud});
    if (!glow || open <= 0.0) {
        return {};
    }
    return {.on = true,
            .level = levelFor(role, glow->brightness * open, peaks, min_levels),
            .color = glow->color};
}

void DaylightProgram::refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now)
{
    if (!now) {
        return; // no time yet: the first ProgramTick will apply it
    }
    forEachLight(role, [&](ev::Role r) { emit(r, *now, kSnapTransition); });
}

void DaylightProgram::on(const ev::ProgramTick &e)
{
    forEachLight(std::nullopt, [&](ev::Role r) { emit(r, e.time, e.transition); });
}

void DaylightProgram::on(const ev::ConfigChanged &e)
{
    config_     = e.config;
    peaks_      = e.peaks;
    min_levels_ = e.min_levels;
}

void DaylightProgram::emit(ev::Role role, const ev::TimeReading &time, ev::Transition transition)
{
    if (time.valid) {
        clock_warned_ = false;
    } else if (!clock_warned_) {
        clock_warned_ = true;
        ev::log::warn(kTag, "Clock not set yet (no time sync) - holding lights off");
    }
    target(role, daylightTarget(role, config_, peaks_, min_levels_, time, cloud_percent_ / 100.0, installation()),
           transition);
}

} // namespace sunsim::core
