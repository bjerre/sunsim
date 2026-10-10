#include "sunsim/core/programs/off_program.h"

namespace sunsim::core {

void OffProgram::refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &)
{
    forEachBulb(role, [&](ev::Role r) { target(r, ev::LightState{.on = false}); });
}

} // namespace sunsim::core
