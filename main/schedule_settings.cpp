#include "schedule_settings.h"

#include <cmath>
#include <cstdio>

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
constexpr const char *kWorkStartKey = "work.start";
constexpr const char *kWorkEndKey   = "work.end";

uint16_t toMinutes(double hours) noexcept
{
    return static_cast<uint16_t>(std::lround(hours * 60.0));
}

void peakKey(char (&key)[16], std::size_t role_index)
{
    const auto name = roleName(role_index);
    std::snprintf(key, sizeof(key), "%.*s.peak", static_cast<int>(name.size()), name.data());
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
    if (nvs_get_u16(handle, kWorkStartKey, &minutes) == ESP_OK) {
        config.work_start_hour = minutes / 60.0;
    }
    if (nvs_get_u16(handle, kWorkEndKey, &minutes) == ESP_OK) {
        config.work_end_hour = minutes / 60.0;
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
        err = nvs_set_u16(handle, kWorkStartKey, toMinutes(config.work_start_hour));
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, kWorkEndKey, toMinutes(config.work_end_hour));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save settings: %s", esp_err_to_name(err));
    }
}

void loadPeakPercents(std::array<uint8_t, kRoleCount> &peaks)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        char    key[16];
        uint8_t percent = 0;
        peakKey(key, i);
        if (nvs_get_u8(handle, key, &percent) == ESP_OK && percent >= 1 && percent <= 100) {
            peaks[i] = percent;
        }
    }
    nvs_close(handle);
}

void savePeakPercent(Role role, uint8_t percent)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to open NVS for writing intensity");
        return;
    }
    char key[16];
    peakKey(key, static_cast<std::size_t>(role));
    esp_err_t err = nvs_set_u8(handle, key, percent);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save intensity: %s", esp_err_to_name(err));
    }
}

} // namespace sunsim
