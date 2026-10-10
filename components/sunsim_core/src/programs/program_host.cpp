#include "sunsim/core/programs/program_host.h"

namespace sunsim::core {

ProgramHost::ProgramHost(const ev::Post &post) : daylight_(post), off_(post), red_alert_(post) {}

Program &ProgramHost::program(ev::Mode mode) noexcept
{
    switch (mode) {
    case ev::Mode::Daylight:
        return daylight_;
    case ev::Mode::Off:
        return off_;
    case ev::Mode::RedAlert:
        return red_alert_;
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

} // namespace sunsim::core
