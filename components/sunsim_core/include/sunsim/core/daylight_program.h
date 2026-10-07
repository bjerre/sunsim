#pragma once

#include <optional>

#include "sunsim/core/daylight.h"
#include "sunsim/ev/events.h"

namespace sunsim::core {

// What each bulb should show right now (LightTarget): the wall bulbs' sun
// curve (with sun direction along the wall and the amber glow), the work
// light's cool -> warm day, the active window, and each light's peak
// intensity. Every ProgramTick ramps over the update interval; RefreshLights
// and setting changes re-apply at once with a quick transition. Until the
// clock has been set (no SNTP yet) every target is "off".
class DaylightProgram {
public:
    explicit DaylightProgram(ev::Post post);

    void onConfig(const ev::ConfigChanged &e);
    void onTick(const ev::Tick &e);
    void onProgramTick(const ev::ProgramTick &e);
    void onRefresh(const ev::RefreshLights &e);

private:
    void emitAll(const ev::TimeReading &time, uint16_t transition_ds);
    void emit(ev::Role role, const ev::TimeReading &time, uint16_t transition_ds);

    ev::Post                       post_;
    DaylightSchedule               schedule_;
    ev::Peaks                      peaks_ = ev::kDefaultPeaks;
    std::optional<ev::TimeReading> time_; // latest, from Tick
    bool                           clock_warned_ = false;
};

} // namespace sunsim::core
