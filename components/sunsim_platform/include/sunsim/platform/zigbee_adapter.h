#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/zha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "sunsim/ev/events.h"

namespace sunsim::platform {

// The only code that talks to esp-zigbee-lib. Runs the stack (its own task),
// forms/rejoins sunsim's network, and translates in both directions:
//   stack -> events : NetworkUp, DeviceAnnounced, EndpointFound, BindResult,
//                     PingResult, AckReceived, ButtonPressed
//   events -> stack : SetLight, Discover, Bind, Ping, OpenNetwork, FactoryReset
// The on*() handlers run on the app task, which holds the Zigbee lock while
// processing events (see lock()/unlock()); the stack callbacks only post.
class ZigbeeAdapter {
public:
    explicit ZigbeeAdapter(ev::Post post);

    ZigbeeAdapter(const ZigbeeAdapter &)            = delete;
    ZigbeeAdapter &operator=(const ZigbeeAdapter &) = delete;

    // Spawns the Zigbee task. waitUntilReady() blocks until the stack (and
    // with it the Zigbee lock) exists.
    void start();
    void waitUntilReady();

    // The stack's API lock, held by the app task around event processing.
    static void lock();
    static void unlock();

    void onSetLight(const ev::SetLight &e);
    void onDiscover(const ev::Discover &e);
    void onBind(const ev::Bind &e);
    void onPing(const ev::Ping &e);
    void onOpenNetwork(const ev::OpenNetwork &e);
    [[noreturn]] void onFactoryReset();
    void onRoleAssigned(const ev::RoleAssigned &e);
    void onRoleCleared(const ev::RoleCleared &e);

private:
    void taskBody();
    void setupCommissioning();
    void createEndpoints();
    void handleSignal(const ezb_app_signal_t *signal);
    void handleZclAction(ezb_zcl_core_action_callback_id_t callback_id, void *message);
    bool handleApsIndication(const ezb_apsde_data_ind_t *ind);
    void scheduleCommissioningRetry(ezb_bdb_comm_mode_mask_t mode, std::chrono::milliseconds delay);
    void retryCommissioning();
    void pressed(ev::Press press);

    static void taskTrampoline(void *arg);
    static bool signalTrampoline(const ezb_app_signal_t *signal);
    static void zclTrampoline(ezb_zcl_core_action_callback_id_t callback_id, void *message);
    static bool apsTrampoline(const ezb_apsde_data_ind_t *ind);
    static void retryTrampoline(void *arg);
    static void onMatchDescResult(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx);
    static void onBindResult(const ezb_zdp_bind_req_result_t *result, void *user_ctx);
    static void onPingResult(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx);

    ev::Post                 post_;
    EventGroupHandle_t       ready_               = nullptr;
    esp_timer_handle_t       retry_timer_         = nullptr;
    ezb_bdb_comm_mode_mask_t pending_retry_mode_  = 0;

    // The button's short address (0xFFFF = none), read by the APS hook on the
    // Zigbee task to decide synchronously whether to consume a frame.
    std::atomic<uint16_t> button_short_{0xFFFF};

    static ZigbeeAdapter *s_self;
};

} // namespace sunsim::platform
