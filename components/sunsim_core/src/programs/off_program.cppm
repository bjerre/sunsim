module;

#include "sunsim/ev/events.h"

export module sunsim.core:off_program;

import std;
import :program;

export namespace sunsim::core {

// Every bulb off until another program is chosen (the button's hold).
class OffProgram final : public Program {
public:
    using Program::Program;

    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now) override;
};

} // namespace sunsim::core
