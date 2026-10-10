module;

#include "sunsim/ev/events.h"
#include "sunsim/ev/log.h"

module sunsim.core;

import std;

namespace sunsim::core {

namespace {
constexpr const char *kTag = "PROGRAM";

// Fixed, short transition for changes that should feel immediate rather
// than mid-animation: resuming, a bulb (re)joining, a setting change.
constexpr ev::Transition kSnapTransition = std::chrono::seconds{2};

// Wall bulbs sit east-to-west along the south wall: A at the east end, C at
// the west end (see DaylightSchedule::wallState).
double wallPosition(ev::Role role) noexcept
{
    switch (role) {
    case ev::Role::WallA:
        return 1.0;
    case ev::Role::WallC:
        return -1.0;
    default:
        return 0.0;
    }
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
    peaks_ = e.peaks;
}

void DaylightProgram::emit(ev::Role role, const ev::TimeReading &time, ev::Transition transition)
{
    ev::LightState state{};
    if (time.valid) {
        clock_warned_ = false;
        state         = role == ev::Role::Work
                            ? schedule_.workState(time.local)
                            : schedule_.wallState(time.local, time.utc_offset_hours, wallPosition(role));
    } else if (!clock_warned_) {
        clock_warned_ = true;
        SUNSIM_LOGW(kTag, "Clock not set yet (no time sync) - holding lights off");
    }
    if (state.on) {
        const long scaled = std::lround(state.level * peaks_[ev::indexOf(role)] / 100.0);
        state.level       = static_cast<uint8_t>(scaled < 1 ? 1 : scaled);
    }
    target(role, state, transition);
}

} // namespace sunsim::core
