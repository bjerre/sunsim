module;

#include "sunsim/ev/log.h"

export module sunsim.core:daylight_program;

import std.compat;
import sunsim.ev;
import :daylight;
import :program;
import :sky;

export namespace sunsim::core {

// The normal program: the wall bulbs' sun curve (with sun direction along
// the wall, the amber glow and the cloud cover: live, else forecast), the work
// light's cool -> warm day, the active window, and each light's range from
// its minimum level to its peak intensity. Every ProgramTick ramps over the
// update interval; refreshes and setting changes re-apply at once with a
// quick transition. Until the clock has been set (no SNTP yet) every bulb
// is held off.
class DaylightProgram final : public Program {
public:
    using Program::Program;

    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now) override;
    void onTick(const ev::Tick &e) override { now_ = e.at; }
    void onProgramTick(const ev::ProgramTick &e) override;
    void onConfig(const ev::ConfigChanged &e) override;
    void onCloudCover(const ev::CloudCoverUpdated &e) override { weather_.onForecast(e); }
    void onConditions(const ev::ConditionsUpdated &e) override { weather_.onConditions(e); }

private:
    void emit(ev::Role role, const ev::TimeReading &time, ev::Transition transition);
    [[nodiscard]] uint8_t outputLevel(ev::Role role, uint8_t level) const noexcept;

    DaylightSchedule schedule_;
    ev::Peaks        peaks_        = ev::kDefaultPeaks;
    ev::MinLevels    min_levels_   = ev::kDefaultMinLevels;
    Weather          weather_;
    ev::Instant      now_{}; // the latest Tick's: whether the live weather is fresh
    bool             clock_warned_ = false;
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
constexpr const char *kTag = "PROGRAM";

// Fixed, short transition for changes that should feel immediate rather
// than mid-animation: resuming, a bulb (re)joining, a setting change.
constexpr ev::Transition kSnapTransition = std::chrono::seconds{2};

// Wall lights are lettered east to west along the wall, evenly spaced: +1
// at the east end, -1 at the west end (see DaylightSchedule::wallState).
double wallPosition(ev::Role role) noexcept
{
    const std::size_t count = ev::members(ev::Group::Wall).size();
    return count < 2 ? 0.0 : 1.0 - 2.0 * role.index / static_cast<double>(count - 1);
}
} // namespace

void DaylightProgram::refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now)
{
    if (!now) {
        return; // no time yet: the first ProgramTick will apply it
    }
    forEachBulb(role, [&](ev::Role r) { emit(r, *now, kSnapTransition); });
}

void DaylightProgram::onProgramTick(const ev::ProgramTick &e)
{
    forEachBulb(std::nullopt, [&](ev::Role r) { emit(r, e.time, e.transition); });
}

void DaylightProgram::onConfig(const ev::ConfigChanged &e)
{
    schedule_.setConfig(e.config);
    peaks_      = e.peaks;
    min_levels_ = e.min_levels;
}

// The schedule's level (min_level..max_level) mapped onto the light's own
// range: its minimum level (the lowest it visibly shows) up to its peak. So
// a light stays lit, dimly, until the curve really ends at sunset.
uint8_t DaylightProgram::outputLevel(ev::Role role, uint8_t level) const noexcept
{
    const ev::ProgramConfig &c     = schedule_.config();
    const double             top   = std::max(1.0, ev::kMaxLevel * peaks_[role] / 100.0);
    const double             floor = std::min<double>(min_levels_[role], top);
    const double             span  = std::max(1, c.max_level - c.min_level);
    const double             f     = std::clamp((level - c.min_level) / span, 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(floor + (top - floor) * f));
}

void DaylightProgram::emit(ev::Role role, const ev::TimeReading &time, ev::Transition transition)
{
    ev::LightState state{};
    if (time.valid) {
        clock_warned_ = false;
        state         = role.group == ev::Group::Work
                            ? schedule_.workState(time.local)
                            : schedule_.wallState(time.local, time.utc_offset_hours, wallPosition(role),
                                                  weather_.cloud(time, now_).value_or(CloudReading{}).cover);
    } else if (!clock_warned_) {
        clock_warned_ = true;
        SUNSIM_LOGW(kTag, "Clock not set yet (no time sync) - holding lights off");
    }
    if (state.on) {
        state.level = outputLevel(role, state.level);
    }
    target(role, state, transition);
}

} // namespace sunsim::core
