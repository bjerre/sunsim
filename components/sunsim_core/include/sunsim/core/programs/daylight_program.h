#pragma once

#include "sunsim/core/daylight.h"
#include "sunsim/core/programs/program.h"

namespace sunsim::core {

// The normal program: the wall bulbs' sun curve (with sun direction along
// the wall and the amber glow), the work light's cool -> warm day, the
// active window, and each light's peak intensity. Every ProgramTick ramps
// over the update interval; refreshes and setting changes re-apply at once
// with a quick transition. Until the clock has been set (no SNTP yet) every
// bulb is held off.
class DaylightProgram final : public Program {
public:
    using Program::Program;

    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now) override;
    void onProgramTick(const ev::ProgramTick &e) override;
    void onConfig(const ev::ConfigChanged &e) override;

private:
    void emit(ev::Role role, const ev::TimeReading &time, uint16_t transition_ds);

    DaylightSchedule schedule_;
    ev::Peaks        peaks_        = ev::kDefaultPeaks;
    bool             clock_warned_ = false;
};

} // namespace sunsim::core
