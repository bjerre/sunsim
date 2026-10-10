module;

#include "sunsim/ev/events.h"

module sunsim.core;

import std;

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

void RedAlertProgram::onTick(const ev::Tick &)
{
    bright_ = !bright_;
    emit(std::nullopt);
}

void RedAlertProgram::emit(std::optional<ev::Role> role)
{
    const ev::LightState red{.on         = true,
                             .level      = bright_ ? kBrightLevel : kDimLevel,
                             .color      = ev::ColorMode::HueSat,
                             .hue        = 0,
                             .saturation = 254};
    forEachBulb(role, [&](ev::Role r) { target(r, red, kFade, kColor); });
}

} // namespace sunsim::core
