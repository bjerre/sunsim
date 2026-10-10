#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "sunsim/ev/events.h"

namespace sunsim::core {

// Turns what the bulbs *should* show (LightTarget, from whichever program is
// running) into the ZCL commands that get them there (SetLight). Only what
// changed since the last command is sent: On/Off on a change, level and
// colour when they move. Bulbs marked unreachable get nothing until they
// answer again (the registry then posts RefreshLights). Bulb addresses come
// from RoleAssigned/RoleCleared.
class LightOutput {
public:
    explicit LightOutput(ev::Post post);

    void onRoleAssigned(const ev::RoleAssigned &e);
    void onRoleCleared(const ev::RoleCleared &e);
    void onReachability(const ev::ReachabilityChanged &e);
    void onTarget(const ev::LightTarget &e);

private:
    struct Bulb {
        bool                          known      = false; // paired with a known endpoint
        uint16_t                      short_addr = 0;
        uint8_t                       endpoint   = 0;
        bool                          reachable  = true;
        std::optional<ev::LightState> sent; // what we last told it; nullopt = unknown, resend everything
    };
    [[nodiscard]] static bool usable(const Bulb &b) noexcept { return b.known && b.reachable; }

    ev::Post                         post_;
    std::array<Bulb, ev::kBulbCount> bulbs_{};
};

} // namespace sunsim::core
