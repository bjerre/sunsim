#pragma once

#include <chrono>

#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "button_controller.h"
#include "light_rig.h"

namespace sunsim {

// Top-level orchestrator: owns the LightRig and ButtonController, wires
// them together, and bridges the plain-C Zigbee/esp_timer callback APIs
// into this instance. One instance for the process's lifetime, reached via
// instance() (a Meyer's singleton) so the static C-callback trampolines
// below have somewhere to forward to.
class SunsimApp {
public:
    static SunsimApp &instance();

    SunsimApp(const SunsimApp &) = delete;
    SunsimApp &operator=(const SunsimApp &) = delete;

    // Loads persisted state, syncs time, and starts the Zigbee stack.
    // Returns once the Zigbee task has been spawned; runs forever after that.
    void run();

private:
    SunsimApp();

    void startHeartbeatTimer();
    void setupCommissioning();
    void createEndpoints();
    void zigbeeTaskBody();

    void handleSignal(const ezb_app_signal_t *signal);
    void handleZclAction(ezb_zcl_core_action_callback_id_t callback_id, void *message);
    void heartbeatTick();
    void scheduleCommissioningRetry(ezb_bdb_comm_mode_mask_t mode, std::chrono::milliseconds delay);
    void retryCommissioning();

    static bool signalTrampoline(const ezb_app_signal_t *signal);
    static void zclTrampoline(ezb_zcl_core_action_callback_id_t callback_id, void *message);
    static void heartbeatTrampoline(void *arg);
    static void retryTrampoline(void *arg);
    static void zigbeeTaskTrampoline(void *arg);
    static void timeSyncTaskTrampoline(void *arg);

    LightRig          light_rig_;
    ButtonController  button_;
    esp_timer_handle_t heartbeat_timer_ = nullptr;
    esp_timer_handle_t retry_timer_     = nullptr;
    ezb_bdb_comm_mode_mask_t pending_retry_mode_   = 0;
    int                heartbeat_ticks_            = 0;
    int                discovery_retry_ticks_      = 0;
    int                ping_ticks_                 = 0;
    int                daylight_tick_seconds_      = 60;
};

} // namespace sunsim
