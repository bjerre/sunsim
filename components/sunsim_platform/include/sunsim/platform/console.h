#pragma once

#include "sunsim/ev/events.h"

namespace sunsim::platform {

class StatusSlot;

// Serial (idf.py monitor) REPL. Commands post requests as events and read
// state from the latest StatusSnapshot; they never touch app state directly.
//
//   status                time, day length, next sunrise/sunset, mode, device links
//   roles                 list role assignments
//   pair <role>           next unassigned device to join gets <role>; opens the network
//   pair cancel           disarm a pending `pair`
//   unpair <role>|all     forget role assignment(s)
//   open [seconds]        open the network for joining (default 180)
//   config                show active/work hours, fade, sun shift, amber, work days
//   config <key> <value>  change one (see `help config`)
//   intensity [<light|all> <1-100>]   per-light peak brightness, saved
//   press single|double|hold          simulate the button
//   wifi [<ssid> [pw]]    show / set the Wi-Fi used for the clock sync
//   wifi clear            forget it, falling back to menuconfig's
//   restart               reboot, keeping everything
//   factory_reset         forget all roles AND the Zigbee network, then restart
//
// <role> is one of WallA, WallB, WallC, Work, Button.
void startConsole(const ev::Post &post, const StatusSlot &status);

} // namespace sunsim::platform
