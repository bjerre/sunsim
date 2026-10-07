#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace sunsim::platform {

// Wi-Fi networks for the SNTP clock sync, e.g. home and work. Saved from
// the console (`wifi add`), stored in NVS so they survive reflashing. When
// none are saved, the menuconfig CONFIG_SUNSIM_WIFI_* network is used.
struct WifiNetwork {
    std::string ssid;
    std::string password; // empty = open network
};

inline constexpr std::size_t kMaxWifiNetworks = 5;

struct WifiNetworks {
    std::vector<WifiNetwork> networks;
    bool                     from_menuconfig = false; // nothing saved; this is the fallback
};

[[nodiscard]] WifiNetworks loadWifiNetworks();

enum class WifiSaveResult { Added, Updated, Invalid, Full, StorageError };
// Adds the network, or updates its password if the SSID is already saved.
WifiSaveResult addWifiNetwork(std::string_view ssid, std::string_view password);
// Returns false if no saved network has that SSID (or storage failed).
bool removeWifiNetwork(std::string_view ssid);
void clearWifiNetworks();

// One clock sync: scans, connects to the strongest saved network in range
// (then any others), syncs the clock over SNTP, then disconnects and stops
// Wi-Fi so it doesn't contend with the 802.15.4 Zigbee radio. One instance
// per sync attempt; safe to run() again later (retries, daily resync).
class WifiTimeSync {
public:
    // Returns true if time was synced, false if it gave up (device still
    // boots and runs, just using whatever wall-clock time it had - likely
    // the 1970 epoch on a device with no RTC battery).
    bool run(std::chrono::milliseconds connect_timeout = std::chrono::milliseconds{15000},
              std::chrono::milliseconds sntp_timeout    = std::chrono::milliseconds{10000});

private:
    // Saved networks to try, best first: those seen in a scan by signal
    // strength, then any not seen (a hidden SSID doesn't show in scans).
    [[nodiscard]] std::vector<WifiNetwork> candidates(const std::vector<WifiNetwork> &saved);
    bool connect(const WifiNetwork &network, std::chrono::milliseconds timeout);

    static void onWifiEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    EventGroupHandle_t event_group_ = nullptr;
    int                retry_count_ = 0;
    bool               connecting_  = false; // reconnect on drop only while trying a network
};

} // namespace sunsim::platform
