#pragma once

namespace sunsim {

// Starts a standalone Touchlink-initiator loop: repeatedly scans for a
// nearby device and sends it a reset-to-factory-new request. Replaces the
// normal sunsim app entirely when CONFIG_SUNSIM_TOUCHLINK_RESET_TOOL is
// enabled - see Kconfig.projbuild. Returns once the background task is
// spawned; runs forever after that.
void runTouchlinkResetTool();

} // namespace sunsim
