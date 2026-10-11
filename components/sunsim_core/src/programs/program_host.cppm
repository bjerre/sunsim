export module sunsim.core:program_host;

import std.compat;
import sunsim.ev;
import :daylight_program;
import :off_program;
import :program;
import :red_alert_program;
import :tuning_program;

export namespace sunsim::core {

// Owns the lighting programs and runs the one the Mode selects: it starts
// it on ModeChanged and forwards ticks and refreshes to it alone. Settings
// and the cloud forecast go to every program. Keeps the latest time, for
// programs that need it when they start or refresh. The console's tuning
// requests go to the tuning program; `tune save` posts its levels as the
// lights' minimums.
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
    void onCloudCover(const ev::CloudCoverUpdated &e);
    void onTune(const ev::TuneLights &e);
    void onTuneSave(const ev::TuneSave &e);

private:
    Program &program(ev::Mode mode) noexcept;
    Program &running() noexcept { return program(mode_); }

    DaylightProgram daylight_;
    OffProgram      off_;
    RedAlertProgram red_alert_;
    TuningProgram   tuning_;
    ev::Post        post_;

    ev::Mode                       mode_ = ev::Mode::Daylight;
    std::optional<ev::TimeReading> time_; // latest, from Tick/ProgramTick
};

} // namespace sunsim::core

namespace sunsim::core {

ProgramHost::ProgramHost(const ev::Post &post)
    : daylight_(post), off_(post), red_alert_(post), tuning_(post), post_(post)
{
}

Program &ProgramHost::program(ev::Mode mode) noexcept
{
    switch (mode) {
    case ev::Mode::Daylight:
        return daylight_;
    case ev::Mode::Off:
        return off_;
    case ev::Mode::RedAlert:
        return red_alert_;
    case ev::Mode::Tuning:
        return tuning_;
    }
    return daylight_;
}

void ProgramHost::onModeChanged(const ev::ModeChanged &e)
{
    mode_ = e.mode;
    running().start(time_);
}

void ProgramHost::onConfig(const ev::ConfigChanged &e)
{
    for (const ev::Mode mode : ev::kModes) {
        program(mode).onConfig(e);
    }
    running().refresh(std::nullopt, time_); // show a change now, not at the next update
}

void ProgramHost::onTick(const ev::Tick &e)
{
    time_ = e.time;
    running().onTick(e);
}

void ProgramHost::onProgramTick(const ev::ProgramTick &e)
{
    time_ = e.time;
    running().onProgramTick(e);
}

void ProgramHost::onRefresh(const ev::RefreshLights &e)
{
    running().refresh(e.role, time_);
}

void ProgramHost::onCloudCover(const ev::CloudCoverUpdated &e)
{
    for (const ev::Mode mode : ev::kModes) {
        program(mode).onCloudCover(e);
    }
}

void ProgramHost::onTune(const ev::TuneLights &e)
{
    tuning_.onTune(e);
}

void ProgramHost::onTuneSave(const ev::TuneSave &)
{
    const auto tuned = tuning_.takeTuned();
    for (const ev::Role role : ev::bulbs()) {
        if (tuned[role]) {
            post_(ev::SetMinLevel{.lights = role, .level = *tuned[role]});
        }
    }
}

} // namespace sunsim::core
