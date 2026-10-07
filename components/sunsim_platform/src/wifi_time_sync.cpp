#include "sunsim/platform/wifi_time_sync.h"

#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"

namespace sunsim::platform {

namespace {

constexpr const char *kTag = "WIFI_TIME";

constexpr int kGotIpBit   = BIT0;
constexpr int kFailBit    = BIT1;
constexpr int kMaxRetries = 5;

constexpr const char *kNamespace   = "sunsim";
constexpr const char *kSsidKey     = "wifi.ssid";
constexpr const char *kPasswordKey = "wifi.pass";

// Sizes of wifi_sta_config_t's ssid/password fields, less the terminator.
constexpr std::size_t kMaxSsidLen     = sizeof(wifi_sta_config_t::ssid) - 1;
constexpr std::size_t kMaxPasswordLen = sizeof(wifi_sta_config_t::password) - 1;

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

WifiCredentials loadWifiCredentials()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
        WifiCredentials creds;
        creds.from_nvs = true;
        const bool      found = readNvsString(handle, kSsidKey, creds.ssid);
        if (found) {
            readNvsString(handle, kPasswordKey, creds.password); // absent = open network
        }
        nvs_close(handle);
        if (found) {
            return creds;
        }
    }
    return {.ssid = CONFIG_SUNSIM_WIFI_SSID, .password = CONFIG_SUNSIM_WIFI_PASSWORD, .from_nvs = false};
}

bool saveWifiCredentials(std::string_view ssid, std::string_view password)
{
    if (ssid.empty() || ssid.size() > kMaxSsidLen || password.size() > kMaxPasswordLen) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(handle, kSsidKey, std::string{ssid}.c_str());
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kPasswordKey, std::string{password}.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

void clearWifiCredentials()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, kSsidKey);
    nvs_erase_key(handle, kPasswordKey);
    nvs_commit(handle);
    nvs_close(handle);
}

void WifiTimeSync::onWifiEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *)
{
    auto *self = static_cast<WifiTimeSync *>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->retry_count_ < kMaxRetries) {
            ++self->retry_count_;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(self->event_group_, kFailBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        self->retry_count_ = 0;
        xEventGroupSetBits(self->event_group_, kGotIpBit);
    }
}

bool WifiTimeSync::run(std::chrono::milliseconds connect_timeout, std::chrono::milliseconds sntp_timeout)
{
    const WifiCredentials creds = loadWifiCredentials();
    if (creds.ssid.empty()) {
        ESP_LOGW(kTag, "No Wi-Fi SSID configured (use the `wifi` console command), skipping time sync");
        return false;
    }
    ESP_LOGI(kTag, "Connecting to \"%s\" (from %s)", creds.ssid.c_str(), creds.from_nvs ? "console" : "menuconfig");
    if (!ensureWifiStackInitialized()) {
        ESP_LOGE(kTag, "Failed to bring up Wi-Fi netif");
        return false;
    }

    event_group_ = xEventGroupCreate();
    retry_count_ = 0;

    esp_event_handler_instance_t wifi_handler_instance;
    esp_event_handler_instance_t ip_handler_instance;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiTimeSync::onWifiEvent, this,
                                         &wifi_handler_instance);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiTimeSync::onWifiEvent, this,
                                         &ip_handler_instance);

    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), creds.ssid.c_str(), kMaxSsidLen);
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), creds.password.c_str(), kMaxPasswordLen);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    const EventBits_t bits = xEventGroupWaitBits(event_group_, kGotIpBit | kFailBit, pdFALSE, pdFALSE,
                                                  pdMS_TO_TICKS(connect_timeout.count()));

    bool synced = false;
    if (bits & kGotIpBit) {
        ESP_LOGI(kTag, "Connected, syncing time over SNTP");
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_cfg);
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(sntp_timeout.count())) == ESP_OK) {
            synced          = true;
            const std::time_t now = std::time(nullptr);
            ESP_LOGI(kTag, "Time synced: %s", std::ctime(&now));
        } else {
            ESP_LOGW(kTag, "SNTP sync timed out");
        }
        esp_netif_sntp_deinit();
    } else {
        ESP_LOGW(kTag, "Wi-Fi connect failed/timed out, proceeding without time sync");
    }

    esp_wifi_disconnect();
    esp_wifi_stop();

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_instance);

    vEventGroupDelete(event_group_);
    event_group_ = nullptr;

    return synced;
}

} // namespace sunsim::platform
