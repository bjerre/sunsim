export module sunsim.core:daylight_program;

import std.compat;
import sunsim.ev;
import :daylight;
import :gate;
import :program;

export namespace sunsim::core {

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
    [[nodiscard]] uint8_t levelFor(ev::Role role, double brightness) const noexcept;

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
} // namespace

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

// A brightness (0-1) on the light's own range: its minimum level (the
// lowest it visibly shows) up to its peak. So a light stays lit, dimly,
// until the curve really ends at sunset.
uint8_t DaylightProgram::levelFor(ev::Role role, double brightness) const noexcept
{
    const double top   = std::max(1.0, ev::kMaxLevel * peaks_[role] / 100.0);
    const double floor = std::min<double>(min_levels_[role], top);
    return static_cast<uint8_t>(std::lround(floor + (top - floor) * std::clamp(brightness, 0.0, 1.0)));
}

void DaylightProgram::emit(ev::Role role, const ev::TimeReading &time, ev::Transition transition)
{
    ev::LightState state{};
    if (time.valid) {
        clock_warned_     = false;
        const double open = openness(ev::spec(zone()).gate, time, config_);
        const auto   glow = show(ev::kLookOf[role], {.config   = config_,
                                                     .time     = time,
                                                     .position = positionInGroup(role),
                                                     .cloud    = cloud_percent_ / 100.0});
        if (glow && open > 0.0) {
            state = {.on = true, .level = levelFor(role, glow->brightness * open), .color = glow->color};
        }
    } else if (!clock_warned_) {
        clock_warned_ = true;
        ev::log::warn(kTag, "Clock not set yet (no time sync) - holding lights off");
    }
    target(role, state, transition);
}

} // namespace sunsim::core
