#pragma once

#include <chrono>
#include <functional>

namespace sunsim {

enum class LightingMode { Auto, ManualOff, RedAlert };

// Decides what a physical single-button Zigbee controller's presses mean,
// decoupled from Zigbee/lighting specifics via injected callbacks:
//   - single press (On/Toggle)  -> resume the daylight program
//   - double press (On/Toggle within the debounce window, or a Scenes
//     RecallScene command) -> red alert
//   - long press / hold (Off)   -> manual all-off
//
// This is a best-guess mapping for common simple Zigbee buttons; adjust
// SunsimApp's ZCL dispatch once you've seen your actual button's traffic
// logged (see README).
class ButtonController {
public:
    // Zigbee locking contract for whoever implements these: resumeAuto and
    // redAlertTick only ever fire from tick(), i.e. from SunsimApp's
    // heartbeat timer (outside any Zigbee callback - the caller must hold
    // the Zigbee lock, as SunsimApp::heartbeatTick does). manualOff and
    // enterRedAlert fire from the press handlers below (onSinglePress() etc.,
    // onOffOffCommand(), onSceneRecall()) - and resumeAuto also from
    // onSinglePress() -
    // i.e. from SunsimApp's ZCL action handler (already a Zigbee callback -
    // do NOT take the lock there) or from the console's `press` command
    // (which takes the lock itself). If you add new call sites for these
    // methods, preserve that split or update both ends.
    struct Callbacks {
        std::function<void()> resumeAuto;
        std::function<void()> manualOff;
        std::function<void()> enterRedAlert;
        std::function<void()> redAlertTick; // called once per heartbeat while in RedAlert
    };

    explicit ButtonController(Callbacks callbacks,
                               std::chrono::milliseconds double_click_window = std::chrono::milliseconds{450});

    // For buttons that say which press it was (e.g. SONOFF SNZB-01:
    // single = Toggle, double = On, hold = Off), no timing needed.
    // Must be called from within a Zigbee callback (see Callbacks above).
    void onSinglePress();
    void onDoublePress();
    void onHold();

    // Call when the button's bound On/Off attribute changes (On/Toggle -> true, Off -> false).
    // Must be called from within a Zigbee callback (see Callbacks above).
    void onOffOffCommand(bool on);
    // Call when the button's bound Scenes RecallScene command arrives.
    // Must be called from within a Zigbee callback (see Callbacks above).
    void onSceneRecall();
    // Call once per heartbeat to resolve the single/double-click debounce
    // window and drive the red-alert effect while active. Must be called
    // with the Zigbee lock held (see Callbacks above).
    void tick();

    [[nodiscard]] LightingMode mode() const noexcept { return mode_; }

private:
    void enterAuto();
    void enterManualOff();
    void enterRedAlert();

    Callbacks                            callbacks_;
    std::chrono::milliseconds            double_click_window_;
    LightingMode                         mode_ = LightingMode::Auto;
    bool                                 click_pending_ = false;
    std::chrono::steady_clock::time_point last_click_{};
};

} // namespace sunsim
