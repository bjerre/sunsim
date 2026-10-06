#pragma once

namespace sunsim {

class ButtonController;
class LightRig;

// Serial (idf.py monitor) REPL for pairing devices to roles and setup:
//
//   status                time, day length, next sunrise/sunset, mode, device links
//   roles                 list role assignments
//   pair <role>           next unassigned device to join gets <role>; opens the network
//   pair cancel           disarm a pending `pair`
//   unpair <role>|all     forget role assignment(s)
//   open [seconds]        open the network for joining (default 180)
//   factory_reset         forget all roles AND the Zigbee network, then restart
//   config                show active hours, edge fade and work days
//   config hours|fade|days <value>   change one (see `help config`)
//   intensity [<light|all> <1-100>]   per-light peak brightness, saved
//   press single|double|hold   simulate the button (resume / red alert / all off)
//   wifi [<ssid> [pw]]    show / set the Wi-Fi used for the boot-time clock sync
//   wifi clear            forget it, falling back to menuconfig's
//   restart               reboot, keeping everything
//
// <role> is one of WallA, WallB, WallC, Work, Button.
void startPairingConsole(LightRig &rig, ButtonController &button);

} // namespace sunsim
