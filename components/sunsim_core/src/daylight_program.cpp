#include "sunsim/core/daylight_program.h"

#include <cmath>

#include "sunsim/ev/log.h"

namespace sunsim::core {

namespace {
constexpr const char *kTag = "PROGRAM";

// Fixed, short transition for changes that should feel immediate rather
// than mid-animation: resuming, a bulb (re)joining, a setting change.
constexpr uint16_t kSnapTransitionDs = 20; // 2.0 s

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

DaylightProgram::DaylightProgram(ev::Post post) : post_(std::move(post)) {}

void DaylightProgram::onConfig(const ev::ConfigChanged &e)
{
    schedule_.setConfig(e.config);
    peaks_ = e.peaks;
    if (time_) {
        emitAll(*time_, kSnapTransitionDs); // show a change now, not at the next update
    }
}

void DaylightProgram::onTick(const ev::Tick &e)
{
    time_ = e.time;
}

void DaylightProgram::onProgramTick(const ev::ProgramTick &e)
{
    time_ = e.time;
    emitAll(e.time, e.transition_ds);
}

void DaylightProgram::onRefresh(const ev::RefreshLights &e)
{
    if (!time_) {
        return; // the first ProgramTick will apply it
    }
    if (e.role) {
        emit(*e.role, *time_, kSnapTransitionDs);
    } else {
        emitAll(*time_, kSnapTransitionDs);
    }
}

void DaylightProgram::emitAll(const ev::TimeReading &time, uint16_t transition_ds)
{
    for (std::size_t i = 0; i < ev::kBulbCount; ++i) {
        emit(static_cast<ev::Role>(i), time, transition_ds);
    }
}

void DaylightProgram::emit(ev::Role role, const ev::TimeReading &time, uint16_t transition_ds)
{
    if (!ev::isBulb(role)) {
        return;
    }
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
    post_(ev::LightTarget{role, state, transition_ds});
}

} // namespace sunsim::core
