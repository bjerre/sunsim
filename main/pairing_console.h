#pragma once

namespace sunsim {

class LightRig;

// Serial (idf.py monitor) REPL for pairing devices to roles:
//
//   roles                 list role assignments
//   pair <role>           next unassigned device to join gets <role>; opens the network
//   pair cancel           disarm a pending `pair`
//   unpair <role>|all     forget role assignment(s)
//   open [seconds]        open the network for joining (default 180)
//   factory_reset         forget all roles AND the Zigbee network, then restart
//
// <role> is one of WallA, WallB, WallC, Work, Button.
void startPairingConsole(LightRig &rig);

} // namespace sunsim
