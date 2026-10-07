#pragma once

#include <array>
#include <cstdint>

#include "sunsim/ev/events.h"

namespace sunsim::core {

// Turns what the bulbs *should* show into the ZCL commands that get them
// there (SetLight), per mode:
//   Auto      - forwards LightTargets, diffed against what was last sent so
//               On/Off only goes out on a change while level/colour ramp
//               every update.
//   ManualOff - switches every bulb off once; ignores targets.
//   RedAlert  - drives RedAlertPulses (red, bright/dim); ignores targets.
// Re-entering Auto posts RefreshLights so the program re-applies at once.
// Bulb addresses come from RoleAssigned/RoleCleared.
class LightOutput {
public:
    explicit LightOutput(ev::Post post);

    void onRoleAssigned(const ev::RoleAssigned &e);
    void onRoleCleared(const ev::RoleCleared &e);
    void onModeChanged(const ev::ModeChanged &e);
    void onRedAlertPulse(const ev::RedAlertPulse &e);
    void onTarget(const ev::LightTarget &e);

private:
    struct Bulb {
        bool     known      = false; // paired with a known endpoint
        uint16_t short_addr = 0;
        uint8_t  endpoint   = 0;
        bool     sent_valid = false; // what we last told it, if anything
        bool     sent_on    = false;
    };
    [[nodiscard]] ev::SetLight to(const Bulb &b) const;

    ev::Post                            post_;
    ev::Mode                            mode_ = ev::Mode::Auto;
    std::array<Bulb, ev::kBulbCount>    bulbs_{};
};

} // namespace sunsim::core
