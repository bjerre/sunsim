#pragma once

namespace sunsim::platform {

class StatusSlot;

// Drives the on-board ST7789 LCD (Waveshare ESP32-C6-GEEK by default, see
// the "Status display" menuconfig): current time, day length, countdown to
// the next sunrise/sunset, mode, and one dot per role for its link state.
// Redraws once a second from the latest StatusSnapshot - the same data as
// the `status` console command. No-op unless CONFIG_SUNSIM_DISPLAY.
void startStatusDisplay(const StatusSlot &status);

} // namespace sunsim::platform
