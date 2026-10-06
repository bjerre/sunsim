#pragma once

namespace sunsim {

class ButtonController;
class LightRig;

// Drives the on-board ST7789 LCD (Waveshare ESP32-C6-GEEK by default, see
// the "Status display" menuconfig): current time, day length, countdown to
// the next sunrise/sunset, mode, and one dot per role for its link state.
// Redraws once a second from takeStatusSnapshot(), the same data as the
// `status` console command. No-op unless CONFIG_SUNSIM_DISPLAY.
void startStatusDisplay(LightRig &rig, ButtonController &button);

} // namespace sunsim
