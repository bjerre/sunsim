export module sunsim.core:off_program;

import std.compat;
import sunsim.ev;
import :program;

export namespace sunsim::core {

// Every bulb off until another program is chosen (the button's hold).
class OffProgram final : public Program {
public:
    static constexpr ev::Mode kMode = ev::Mode::Off;
    using Program::Program;

    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now);
};

} // namespace sunsim::core

namespace sunsim::core {

void OffProgram::refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &)
{
    forEachBulb(role, [&](ev::Role r) { target(r, ev::LightState{.on = false}); });
}

} // namespace sunsim::core
