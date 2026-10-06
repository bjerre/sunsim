#include "schedule_settings.h"

#include <cmath>

#include "esp_log.h"
#include "nvs.h"

namespace sunsim {

namespace {
constexpr const char *kTag       = "SETTINGS";
constexpr const char *kNamespace = "sunsim";

// Hours/fade are stored as whole minutes.
constexpr const char *kStartKey = "act.start";
constexpr const char *kEndKey   = "act.end";
constexpr const char *kFadeKey  = "act.fade";
constexpr const char *kDaysKey  = "act.days";

uint16_t toMinutes(double hours) noexcept
{
    return static_cast<uint16_t>(std::lround(hours * 60.0));
}
} // namespace

void loadScheduleSettings(DaylightSchedule::Config &config)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint16_t minutes = 0;
    if (nvs_get_u16(handle, kStartKey, &minutes) == ESP_OK) {
        config.active_start_hour = minutes / 60.0;
    }
    if (nvs_get_u16(handle, kEndKey, &minutes) == ESP_OK) {
        config.active_end_hour = minutes / 60.0;
    }
    if (nvs_get_u16(handle, kFadeKey, &minutes) == ESP_OK) {
        config.fade_minutes = minutes;
    }
    uint8_t days = 0;
    if (nvs_get_u8(handle, kDaysKey, &days) == ESP_OK) {
        config.active_days = days;
    }
    nvs_close(handle);
}

void saveScheduleSettings(const DaylightSchedule::Config &config)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to open NVS for writing settings");
        return;
    }
    esp_err_t err = nvs_set_u16(handle, kStartKey, toMinutes(config.active_start_hour));
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, kEndKey, toMinutes(config.active_end_hour));
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, kFadeKey, static_cast<uint16_t>(std::lround(config.fade_minutes)));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, kDaysKey, config.active_days);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save settings: %s", esp_err_to_name(err));
    }
}

} // namespace sunsim
