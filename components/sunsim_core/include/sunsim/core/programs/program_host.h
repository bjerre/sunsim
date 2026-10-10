#pragma once

#include <array>
#include <optional>

#include "sunsim/core/programs/daylight_program.h"
#include "sunsim/core/programs/off_program.h"
#include "sunsim/core/programs/program.h"
#include "sunsim/core/programs/red_alert_program.h"

namespace sunsim::core {

// Owns the lighting programs and runs the one the Mode selects: it starts
// it on ModeChanged and forwards ticks and refreshes to it alone. Settings
// go to every program. Keeps the latest time, for programs that need it
// when they start or refresh.
//
// Adding a program: a Mode (sunsim::ev), a Program subclass, a member and
// its case in program() here, and whatever selects it (ModeController,
// console).
class ProgramHost {
public:
    explicit ProgramHost(const ev::Post &post);

    void onModeChanged(const ev::ModeChanged &e);
    void onConfig(const ev::ConfigChanged &e);
    void onTick(const ev::Tick &e);
    void onProgramTick(const ev::ProgramTick &e);
    void onRefresh(const ev::RefreshLights &e);

private:
    Program &program(ev::Mode mode) noexcept;
    Program &running() noexcept { return program(mode_); }

    DaylightProgram daylight_;
    OffProgram      off_;
    RedAlertProgram red_alert_;
    std::array<Program *, 3> all_{&daylight_, &off_, &red_alert_};

    ev::Mode                       mode_ = ev::Mode::Daylight;
    std::optional<ev::TimeReading> time_; // latest, from Tick/ProgramTick
};

} // namespace sunsim::core
