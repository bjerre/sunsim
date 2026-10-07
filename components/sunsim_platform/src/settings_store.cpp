#include "sunsim/platform/settings_store.h"

#include <cmath>
#include <cstdio>

#include "esp_log.h"
#include "nvs.h"

namespace sunsim::platform {

namespace {
constexpr const char *kTag       = "SETTINGS";
constexpr const char *kNamespace = "sunsim";

// Pre-versioning format: the whole roles array as one raw-struct blob. Only
// erased now, never read.
constexpr const char *kLegacyBlobKey = "roles";

// Program settings; hours/fade are stored as whole minutes.
constexpr const char *kStartKey     = "act.start";
constexpr const char *kEndKey       = "act.end";
constexpr const char *kFadeKey      = "act.fade";
constexpr const char *kDaysKey      = "act.days";
constexpr const char *kWorkStartKey = "work.start";
constexpr const char *kWorkEndKey   = "work.end";
constexpr const char *kSunShiftKey  = "sun.shift";
constexpr const char *kAmberKey     = "sun.amber";

uint16_t toMinutes(double hours) noexcept
{
    return static_cast<uint16_t>(std::lround(hours * 60.0));
}

// "<role>.<field>", at most 15 chars (NVS key limit).
struct Key {
    char text[16];
    Key(std::size_t role_index, const char *field)
    {
        const auto name = ev::roleName(role_index);
        std::snprintf(text, sizeof(text), "%.*s.%s", static_cast<int>(name.size()), name.data(), field);
    }
};

class Nvs {
public:
    explicit Nvs(nvs_open_mode_t mode) : ok_(nvs_open(kNamespace, mode, &handle_) == ESP_OK) {}
    ~Nvs()
    {
        if (ok_) {
            nvs_close(handle_);
        }
    }
    Nvs(const Nvs &)            = delete;
    Nvs &operator=(const Nvs &) = delete;

    [[nodiscard]] bool         ok() const noexcept { return ok_; }
    [[nodiscard]] nvs_handle_t get() const noexcept { return handle_; }

private:
    nvs_handle_t handle_{};
    bool         ok_;
};

void logError(const char *what, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save %s: %s", what, esp_err_to_name(err));
    }
}
} // namespace

SettingsStore::SettingsStore(ev::Post post, ev::ProgramConfig defaults) : post_(std::move(post)), config_(defaults) {}

void SettingsStore::publish()
{
    post_(ev::ConfigChanged{.config = config_, .peaks = peaks_});
}

void SettingsStore::loadAll()
{
    ev::RolesLoaded roles{};

    Nvs nvs{NVS_READONLY};
    if (!nvs.ok()) {
        ESP_LOGI(kTag, "Nothing stored yet");
    } else {
        const nvs_handle_t h = nvs.get();

        for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
            ev::DeviceRecord &d = roles.records[i];
            if (nvs_get_u64(h, Key(i, "ext").text, &d.ext_addr) != ESP_OK) {
                continue;
            }
            d.valid = true;
            nvs_get_u16(h, Key(i, "short").text, &d.short_addr); // 0 if missing; refreshed on next announce
            nvs_get_u8(h, Key(i, "ep").text, &d.endpoint);       // 0 if missing; triggers rediscovery
            ESP_LOGI(kTag, "Restored %s: ext=0x%016llx short=0x%04x ep=%d", ev::roleName(i).data(),
                     static_cast<unsigned long long>(d.ext_addr), d.short_addr, d.endpoint);
        }

        uint16_t minutes = 0;
        if (nvs_get_u16(h, kStartKey, &minutes) == ESP_OK) {
            config_.active_start_hour = minutes / 60.0;
        }
        if (nvs_get_u16(h, kEndKey, &minutes) == ESP_OK) {
            config_.active_end_hour = minutes / 60.0;
        }
        if (nvs_get_u16(h, kFadeKey, &minutes) == ESP_OK) {
            config_.fade_minutes = minutes;
        }
        if (nvs_get_u16(h, kWorkStartKey, &minutes) == ESP_OK) {
            config_.work_start_hour = minutes / 60.0;
        }
        if (nvs_get_u16(h, kWorkEndKey, &minutes) == ESP_OK) {
            config_.work_end_hour = minutes / 60.0;
        }
        uint8_t byte = 0;
        if (nvs_get_u8(h, kDaysKey, &byte) == ESP_OK) {
            config_.active_days = byte;
        }
        if (nvs_get_u8(h, kSunShiftKey, &byte) == ESP_OK && byte <= 100) {
            config_.sun_shift_percent = byte;
        }
        if (nvs_get_u8(h, kAmberKey, &byte) == ESP_OK) {
            config_.amber_enabled = byte != 0;
        }
        for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
            if (nvs_get_u8(h, Key(i, "peak").text, &byte) == ESP_OK && byte >= 1 && byte <= 100) {
                peaks_[i] = byte;
            }
        }
    }

    post_(roles);
    publish();
}

void SettingsStore::onSetConfig(const ev::SetConfig &e)
{
    config_ = e.config;

    Nvs nvs{NVS_READWRITE};
    if (!nvs.ok()) {
        ESP_LOGE(kTag, "Failed to open NVS for writing settings");
    } else {
        const nvs_handle_t h   = nvs.get();
        esp_err_t          err = nvs_set_u16(h, kStartKey, toMinutes(config_.active_start_hour));
        if (err == ESP_OK) {
            err = nvs_set_u16(h, kEndKey, toMinutes(config_.active_end_hour));
        }
        if (err == ESP_OK) {
            err = nvs_set_u16(h, kFadeKey, static_cast<uint16_t>(std::lround(config_.fade_minutes)));
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, kDaysKey, config_.active_days);
        }
        if (err == ESP_OK) {
            err = nvs_set_u16(h, kWorkStartKey, toMinutes(config_.work_start_hour));
        }
        if (err == ESP_OK) {
            err = nvs_set_u16(h, kWorkEndKey, toMinutes(config_.work_end_hour));
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, kSunShiftKey, config_.sun_shift_percent);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, kAmberKey, config_.amber_enabled ? 1 : 0);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        logError("settings", err);
    }
    publish();
}

void SettingsStore::onSetIntensity(const ev::SetIntensity &e)
{
    const uint8_t percent = e.percent < 1 ? 1 : (e.percent > 100 ? 100 : e.percent);

    Nvs       nvs{NVS_READWRITE};
    esp_err_t err = nvs.ok() ? ESP_OK : ESP_FAIL;
    for (std::size_t i = 0; i < ev::kBulbCount; ++i) {
        if (e.role && ev::indexOf(*e.role) != i) {
            continue;
        }
        peaks_[i] = percent;
        if (err == ESP_OK) {
            err = nvs_set_u8(nvs.get(), Key(i, "peak").text, percent);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs.get());
    }
    logError("intensity", err);
    publish();
}

void SettingsStore::onPersistRoles(const ev::PersistRoles &e)
{
    Nvs nvs{NVS_READWRITE};
    if (!nvs.ok()) {
        ESP_LOGE(kTag, "Failed to open NVS for writing roles");
        return;
    }
    const nvs_handle_t h = nvs.get();
    nvs_erase_key(h, kLegacyBlobKey); // ESP_ERR_NVS_NOT_FOUND once gone; harmless

    esp_err_t err = ESP_OK;
    for (std::size_t i = 0; i < ev::kRoleCount && err == ESP_OK; ++i) {
        const ev::DeviceRecord &d = e.records[i];
        if (d.valid) {
            err = nvs_set_u64(h, Key(i, "ext").text, d.ext_addr);
            if (err == ESP_OK) {
                err = nvs_set_u16(h, Key(i, "short").text, d.short_addr);
            }
            if (err == ESP_OK) {
                err = nvs_set_u8(h, Key(i, "ep").text, d.endpoint);
            }
        } else {
            nvs_erase_key(h, Key(i, "ext").text);
            nvs_erase_key(h, Key(i, "short").text);
            nvs_erase_key(h, Key(i, "ep").text);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    logError("roles", err);
}

void SettingsStore::onFactoryReset()
{
    ESP_LOGW(kTag, "Clearing all roles");
    onPersistRoles(ev::PersistRoles{}); // all records invalid -> keys erased
}

} // namespace sunsim::platform
