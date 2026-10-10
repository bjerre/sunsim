module;

#include "sunsim/ev/events.h"

export module sunsim.core:program_host;

import std;
import :daylight_program;
import :off_program;
import :program;
import :red_alert_program;

export namespace sunsim::core {

// Owns the lighting programs and runs the one the Mode selects: it starts
// it on ModeChanged and forwards ticks and refreshes to it alone. Settings
// go to every program. Keeps the latest time, for programs that need it
// when they start or refresh.
//
// Adding a program: a Mode (in sunsim::ev: the enum, kModes, modeName), a
// Program subclass, a member and its case in program() here, and whatever
// selects it (ModeController, console).
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

    ev::Mode                       mode_ = ev::Mode::Daylight;
    std::optional<ev::TimeReading> time_; // latest, from Tick/ProgramTick
};

} // namespace sunsim::core
