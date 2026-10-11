module;

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

module sunsim.platform:wifi_time_sync; // internal: not exported

import std.compat;

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

namespace sunsim::platform {

namespace {

constexpr const char *kTag = "WIFI_TIME";

constexpr int kGotIpBit   = BIT0;
constexpr int kFailBit    = BIT1;
constexpr int kMaxRetries = 3; // per network, before moving on to the next

constexpr const char *kNamespace = "sunsim";

// Pre-multi-network keys (one network). Read as part of the list; moved
// into the numbered slots the first time the list is changed.
constexpr const char *kLegacySsidKey     = "wifi.ssid";
constexpr const char *kLegacyPasswordKey = "wifi.pass";

// Sizes of wifi_sta_config_t's ssid/password fields, less the terminator.
constexpr std::size_t kMaxSsidLen     = sizeof(wifi_sta_config_t::ssid) - 1;
constexpr std::size_t kMaxPasswordLen = sizeof(wifi_sta_config_t::password) - 1;

constexpr uint16_t kMaxScanRecords = 20;

// Slot keys "wifi<n>.ssid" / "wifi<n>.pass".
struct SlotKeys {
    char ssid[16];
    char pass[16];
    explicit SlotKeys(std::size_t slot)
    {
        std::snprintf(ssid, sizeof(ssid), "wifi%u.ssid", static_cast<unsigned>(slot));
        std::snprintf(pass, sizeof(pass), "wifi%u.pass", static_cast<unsigned>(slot));
    }
};

bool readNvsString(nvs_handle_t handle, const char *key, std::string &out)
{
    std::size_t len = 0;
    if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len == 0) {
        return false;
    }
    out.resize(len);
    if (nvs_get_str(handle, key, out.data(), &len) != ESP_OK) {
        return false;
    }
    out.resize(len - 1); // drop the terminator nvs_get_str counts
    return true;
}

std::vector<WifiNetwork> readSaved()
{
    std::vector<WifiNetwork> saved;
    nvs_handle_t             handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return saved;
    }
    for (std::size_t i = 0; i < kMaxWifiNetworks; ++i) {
        const SlotKeys k{i};
        WifiNetwork    n;
        if (readNvsString(handle, k.ssid, n.ssid)) {
            readNvsString(handle, k.pass, n.password); // absent = open network
            saved.push_back(std::move(n));
        }
    }
    WifiNetwork legacy;
    if (readNvsString(handle, kLegacySsidKey, legacy.ssid)) {
        readNvsString(handle, kLegacyPasswordKey, legacy.password);
        const bool listed = std::any_of(saved.begin(), saved.end(),
                                        [&](const WifiNetwork &n) { return n.ssid == legacy.ssid; });
        if (!listed && saved.size() < kMaxWifiNetworks) {
            saved.insert(saved.begin(), std::move(legacy));
        }
    }
    nvs_close(handle);
    return saved;
}

bool writeSaved(const std::vector<WifiNetwork> &saved)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = ESP_OK;
    for (std::size_t i = 0; i < kMaxWifiNetworks && err == ESP_OK; ++i) {
        const SlotKeys k{i};
        if (i < saved.size()) {
            err = nvs_set_str(handle, k.ssid, saved[i].ssid.c_str());
            if (err == ESP_OK) {
                err = nvs_set_str(handle, k.pass, saved[i].password.c_str());
            }
        } else {
            nvs_erase_key(handle, k.ssid);
            nvs_erase_key(handle, k.pass);
        }
    }
    nvs_erase_key(handle, kLegacySsidKey); // migrated into the slots now
    nvs_erase_key(handle, kLegacyPasswordKey);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool ensureWifiStackInitialized()
{
    static bool initialized = false;
    if (initialized) {
        return true;
    }
    if (esp_netif_init() != ESP_OK) {
        return false;
    }
    const esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }

    initialized = true;
    return true;
}

} // namespace

WifiNetworks loadWifiNetworks()
{
    WifiNetworks result{.networks = readSaved(), .from_menuconfig = false};
    if (result.networks.empty() && std::strlen(CONFIG_SUNSIM_WIFI_SSID) > 0) {
        result.networks.push_back({.ssid = CONFIG_SUNSIM_WIFI_SSID, .password = CONFIG_SUNSIM_WIFI_PASSWORD});
        result.from_menuconfig = true;
    }
    return result;
}

WifiSaveResult addWifiNetwork(std::string_view ssid, std::string_view password)
{
    if (ssid.empty() || ssid.size() > kMaxSsidLen || password.size() > kMaxPasswordLen) {
        return WifiSaveResult::Invalid;
    }
    std::vector<WifiNetwork> saved = readSaved();
    auto it = std::find_if(saved.begin(), saved.end(), [&](const WifiNetwork &n) { return n.ssid == ssid; });
    WifiSaveResult result = WifiSaveResult::Updated;
    if (it != saved.end()) {
        it->password = std::string{password};
    } else if (saved.size() >= kMaxWifiNetworks) {
        return WifiSaveResult::Full;
    } else {
        saved.push_back({.ssid = std::string{ssid}, .password = std::string{password}});
        result = WifiSaveResult::Added;
    }
    return writeSaved(saved) ? result : WifiSaveResult::StorageError;
}

bool removeWifiNetwork(std::string_view ssid)
{
    std::vector<WifiNetwork> saved = readSaved();
    const auto               it =
        std::remove_if(saved.begin(), saved.end(), [&](const WifiNetwork &n) { return n.ssid == ssid; });
    if (it == saved.end()) {
        return false;
    }
    saved.erase(it, saved.end());
    return writeSaved(saved);
}

void clearWifiNetworks()
{
    writeSaved({});
}

void WifiTimeSync::onWifiEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    auto *self = static_cast<WifiTimeSync *>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!self->connecting_) {
            return; // our own disconnect between networks, or after the sync
        }
        // e.g. 201 = no AP found, 15/204 = handshake failed (usually a wrong password)
        const auto *d = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        ESP_LOGI(kTag, "  attempt %d failed (reason %d)", self->retry_count_ + 1, d ? d->reason : -1);
        if (self->retry_count_ < kMaxRetries) {
            ++self->retry_count_;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(self->event_group_, kFailBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(self->event_group_, kGotIpBit);
    }
}

std::vector<WifiNetwork> WifiTimeSync::candidates(const std::vector<WifiNetwork> &saved)
{
    if (saved.size() == 1) {
        return saved; // nothing to choose between; skip the scan
    }

    std::vector<wifi_ap_record_t> records(kMaxScanRecords);
    uint16_t                      count = kMaxScanRecords;
    if (esp_wifi_scan_start(nullptr, true) != ESP_OK ||
        esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        ESP_LOGW(kTag, "Scan failed; trying saved networks in order");
        return saved;
    }
    records.resize(count); // the driver returns them strongest first

    std::vector<WifiNetwork> seen;
    for (const wifi_ap_record_t &r : records) {
        const std::string_view ssid{reinterpret_cast<const char *>(r.ssid)};
        for (const WifiNetwork &n : saved) {
            const bool already = std::any_of(seen.begin(), seen.end(),
                                             [&](const WifiNetwork &s) { return s.ssid == n.ssid; });
            if (n.ssid == ssid && !already) {
                ESP_LOGI(kTag, "In range: \"%s\" (%d dBm)", n.ssid.c_str(), r.rssi);
                seen.push_back(n);
            }
        }
    }
    if (seen.empty()) {
        ESP_LOGW(kTag, "No saved network seen in the scan; trying each anyway (it may be hidden)");
        // What *is* around, to tell "out of range" from a typo or a 5 GHz-only
        // network (the C6 is 2.4 GHz only).
        ESP_LOGI(kTag, "Scan found %u network(s)%s", static_cast<unsigned>(records.size()),
                 records.empty() ? "" : ", strongest first:");
        for (std::size_t i = 0; i < records.size() && i < 8; ++i) {
            ESP_LOGI(kTag, "  \"%s\" (%d dBm, channel %d)", reinterpret_cast<const char *>(records[i].ssid),
                     records[i].rssi, records[i].primary);
        }
        return saved;
    }
    return seen;
}

bool WifiTimeSync::connect(const WifiNetwork &network, std::chrono::milliseconds timeout)
{
    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), network.ssid.c_str(), kMaxSsidLen);
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), network.password.c_str(), kMaxPasswordLen);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    ESP_LOGI(kTag, "Connecting to \"%s\"", network.ssid.c_str());
    xEventGroupClearBits(event_group_, kGotIpBit | kFailBit);
    retry_count_ = 0;
    connecting_  = true;
    esp_wifi_connect();

    const EventBits_t bits =
        xEventGroupWaitBits(event_group_, kGotIpBit | kFailBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout.count()));
    connecting_ = false;
    if (bits & kGotIpBit) {
        return true;
    }
    ESP_LOGW(kTag, "Could not connect to \"%s\"", network.ssid.c_str());
    esp_wifi_disconnect();
    return false;
}

bool WifiTimeSync::run(std::chrono::milliseconds connect_timeout, std::chrono::milliseconds sntp_timeout)
{
    const WifiNetworks saved = loadWifiNetworks();
    if (saved.networks.empty()) {
        ESP_LOGW(kTag, "No Wi-Fi network configured (use `wifi add <ssid> <password>`), skipping time sync");
        return false;
    }
    if (!ensureWifiStackInitialized()) {
        ESP_LOGE(kTag, "Failed to bring up Wi-Fi netif");
        return false;
    }

    event_group_ = xEventGroupCreate();

    esp_event_handler_instance_t wifi_handler_instance;
    esp_event_handler_instance_t ip_handler_instance;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiTimeSync::onWifiEvent, this,
                                         &wifi_handler_instance);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiTimeSync::onWifiEvent, this,
                                         &ip_handler_instance);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    bool connected = false;
    for (const WifiNetwork &n : candidates(saved.networks)) {
        if (connect(n, connect_timeout)) {
            connected = true;
            break;
        }
    }

    bool synced = false;
    if (connected) {
        ESP_LOGI(kTag, "Connected, syncing time over SNTP");
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_cfg);
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(sntp_timeout.count())) == ESP_OK) {
            synced                = true;
            const std::time_t now = std::time(nullptr);
            ESP_LOGI(kTag, "Time synced: %s", std::ctime(&now));
        } else {
            ESP_LOGW(kTag, "SNTP sync timed out");
        }
        esp_netif_sntp_deinit();
    } else {
        ESP_LOGW(kTag, "No Wi-Fi network reachable, proceeding without time sync");
    }

    connecting_ = false;
    esp_wifi_disconnect();
    esp_wifi_stop();

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_instance);

    vEventGroupDelete(event_group_);
    event_group_ = nullptr;

    return synced;
}

} // namespace sunsim::platform
