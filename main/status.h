#pragma once

#include <array>
#include <ctime>

#include "button_controller.h"
#include "day_info.h"
#include "light_rig.h"
#include "roles_store.h"

namespace sunsim {

enum class DeviceLink {
    Unpaired, // no device holds this role
    Silent,   // paired, but not heard from recently (or never, for a button)
    Online,   // heard from within kOnlineWindow
};

// Everything the status console command and the on-board display show,
// gathered in one place so both always agree.
struct StatusSnapshot {
    std::tm      time{};
    bool         clock_valid      = false;
    bool         simulated        = false; // debug fast-day clock
    DayInfo      day{};
    LightingMode mode             = LightingMode::Auto;
    bool         in_active_window = false;
    std::array<DeviceLink, kRoleCount>             links{};
    std::array<LightRig::DeviceStatus, kRoleCount> devices{};
};

// Caller must hold the Zigbee lock (reads LightRig/ButtonController state).
[[nodiscard]] StatusSnapshot takeStatusSnapshot(const LightRig &rig, const ButtonController &button);

[[nodiscard]] const char *modeName(LightingMode mode) noexcept;

} // namespace sunsim
