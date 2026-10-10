#pragma once

#include "sunsim/core/programs/program.h"

namespace sunsim::core {

// Every bulb red, pulsing like TNG's red-alert lighting: a slow, continuous
// glow rather than a strobe. It flips between bright and dim every Tick (a
// 2 s cycle, ~0.5 Hz), each fade taking nearly the whole second. Starts
// bright.
class RedAlertProgram final : public Program {
public:
    using Program::Program;

    void start(const std::optional<ev::TimeReading> &now) override;
    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now) override;
    void onTick(const ev::Tick &e) override;

private:
    void emit(std::optional<ev::Role> role);

    bool bright_ = true;
};

} // namespace sunsim::core
