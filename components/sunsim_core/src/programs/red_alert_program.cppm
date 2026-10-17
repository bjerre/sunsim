export module sunsim.core:red_alert_program;

import std.compat;
import sunsim.ev;
import :program;

export namespace sunsim::core {

// Every bulb red, pulsing like TNG's red-alert lighting: a slow, continuous
// glow rather than a strobe. It flips between bright and dim every Tick (a
// 2 s cycle, ~0.5 Hz), each fade taking nearly the whole second. Starts
// bright.
class RedAlertProgram final : public Program {
public:
    static constexpr ev::Mode kMode = ev::Mode::RedAlert;
    using Program::Program;

    void start(const std::optional<ev::TimeReading> &now);
    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now);
    void on(const ev::Tick &e);

private:
    void emit(std::optional<ev::Role> role);

    bool bright_ = true;
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
constexpr ev::Transition kFade{9};  // 0.9 s: nearly the whole Tick
constexpr ev::Transition kColor{2}; // 0.2 s
constexpr uint8_t  kBrightLevel = 254;
constexpr uint8_t  kDimLevel    = 30;
} // namespace

void RedAlertProgram::start(const std::optional<ev::TimeReading> &)
{
    bright_ = true;
    emit(std::nullopt);
}

void RedAlertProgram::refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &)
{
    emit(role);
}

void RedAlertProgram::on(const ev::Tick &)
{
    bright_ = !bright_;
    emit(std::nullopt);
}

void RedAlertProgram::emit(std::optional<ev::Role> role)
{
    const ev::LightState red{.on    = true,
                             .level = bright_ ? kBrightLevel : kDimLevel,
                             .color = ev::HueSat{.hue = 0, .saturation = 254}};
    forEachLight(role, [&](ev::Role r) { target(r, red, kFade, kColor); });
}

} // namespace sunsim::core
