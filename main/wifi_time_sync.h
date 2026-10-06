#pragma once

#include <chrono>

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace sunsim {

// Connects to Wi-Fi (blocking, with its own retry/timeout), syncs the clock
// over SNTP, then disconnects and stops Wi-Fi so it doesn't contend with
// the 802.15.4 Zigbee radio for the rest of the device's uptime. One
// instance per sync attempt; safe to construct and run() again later (e.g.
// from a daily resync timer) to correct clock drift.
class WifiTimeSync {
public:
    // Returns true if time was synced, false if it gave up (device still
    // boots and runs, just using whatever wall-clock time it had - likely
    // the 1970 epoch on a device with no RTC battery).
    bool run(std::chrono::milliseconds connect_timeout = std::chrono::milliseconds{15000},
              std::chrono::milliseconds sntp_timeout    = std::chrono::milliseconds{10000});

private:
    static void onWifiEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    EventGroupHandle_t event_group_ = nullptr;
    int                retry_count_ = 0;
};

} // namespace sunsim
