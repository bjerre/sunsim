module;

#include "esp_log.h"
#include "nvs.h"

export module sunsim.platform:settings_store;

import std.compat;
import sunsim.ev;

export namespace sunsim::platform {

// Everything sunsim persists, in NVS namespace "sunsim", one key per field
// so the format survives firmware changes (a new field just reads as its
// default). Keys are unchanged from the pre-event firmware:
//   roles     "<role>.ext" / ".short" / ".ep"   (a role is assigned iff .ext exists)
//   program   "act.start" "act.end" "act.fade" "act.days" "work.start"
//             "work.end" "sun.shift" "sun.amber"  (hours stored as minutes)
//   intensity "<role>.peak"
// Wi-Fi credentials are kept separately (:wifi_time_sync).
class SettingsStore {
public:
    // `defaults`: the program settings before anything was saved (location
    // comes from menuconfig).
    SettingsStore(ev::Post post, ev::ProgramConfig defaults);

    // Posts RolesLoaded and ConfigChanged with what's stored.
    void loadAll();

    void onSetConfig(const ev::SetConfig &e);
    void onSetIntensity(const ev::SetIntensity &e);
    void onPersistRoles(const ev::PersistRoles &e);
    void onFactoryReset();

private:
    void publish();

    ev::Post          post_;
    ev::ProgramConfig config_;
    ev::Peaks         peaks_ = ev::kDefaultPeaks;
};

} // namespace sunsim::platform

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
    explicit Nvs(nvs_open_mode_t mode) : err_(nvs_open(kNamespace, mode, &handle_)) {}
    ~Nvs()
    {
        if (ok()) {
            nvs_close(handle_);
        }
    }
    Nvs(const Nvs &)            = delete;
    Nvs &operator=(const Nvs &) = delete;

    [[nodiscard]] bool         ok() const noexcept { return err_ == ESP_OK; }
    [[nodiscard]] esp_err_t    error() const noexcept { return err_; }
    [[nodiscard]] nvs_handle_t get() const noexcept { return handle_; }

private:
    nvs_handle_t handle_{};
    esp_err_t    err_;
};

// Writes key after key, skipping the rest once one fails (or NVS didn't
// open); commit() commits if all went well and logs the first error if not.
class NvsWriter {
public:
    NvsWriter(const Nvs &nvs, const char *what) : h_(nvs.get()), what_(what), err_(nvs.error()) {}

    void u8(const char *key, uint8_t value)
    {
        if (ok()) {
            err_ = nvs_set_u8(h_, key, value);
        }
    }
    void u16(const char *key, uint16_t value)
    {
        if (ok()) {
            err_ = nvs_set_u16(h_, key, value);
        }
    }
    void u64(const char *key, uint64_t value)
    {
        if (ok()) {
            err_ = nvs_set_u64(h_, key, value);
        }
    }
    void erase(const char *key)
    {
        if (ok()) {
            nvs_erase_key(h_, key); // ESP_ERR_NVS_NOT_FOUND if it's already gone: fine
        }
    }

    void commit()
    {
        if (ok()) {
            err_ = nvs_commit(h_);
        }
        if (!ok()) {
            ESP_LOGE(kTag, "Failed to save %s: %s", what_, esp_err_to_name(err_));
        }
    }

private:
    [[nodiscard]] bool ok() const noexcept { return err_ == ESP_OK; }

    nvs_handle_t h_;
    const char  *what_;
    esp_err_t    err_;
};
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

    Nvs       nvs{NVS_READWRITE};
    NvsWriter w{nvs, "settings"};
    w.u16(kStartKey, toMinutes(config_.active_start_hour));
    w.u16(kEndKey, toMinutes(config_.active_end_hour));
    w.u16(kFadeKey, static_cast<uint16_t>(std::lround(config_.fade_minutes)));
    w.u8(kDaysKey, config_.active_days);
    w.u16(kWorkStartKey, toMinutes(config_.work_start_hour));
    w.u16(kWorkEndKey, toMinutes(config_.work_end_hour));
    w.u8(kSunShiftKey, config_.sun_shift_percent);
    w.u8(kAmberKey, config_.amber_enabled ? 1 : 0);
    w.commit();
    publish();
}

void SettingsStore::onSetIntensity(const ev::SetIntensity &e)
{
    const uint8_t percent = e.percent < 1 ? 1 : (e.percent > 100 ? 100 : e.percent);

    Nvs       nvs{NVS_READWRITE};
    NvsWriter w{nvs, "intensity"};
    for (std::size_t i = 0; i < ev::kBulbCount; ++i) {
        if (e.role && ev::indexOf(*e.role) != i) {
            continue;
        }
        peaks_[i] = percent;
        w.u8(Key(i, "peak").text, percent);
    }
    w.commit();
    publish();
}

void SettingsStore::onPersistRoles(const ev::PersistRoles &e)
{
    Nvs       nvs{NVS_READWRITE};
    NvsWriter w{nvs, "roles"};
    w.erase(kLegacyBlobKey);
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        const ev::DeviceRecord &d = e.records[i];
        if (d.valid) {
            w.u64(Key(i, "ext").text, d.ext_addr);
            w.u16(Key(i, "short").text, d.short_addr);
            w.u8(Key(i, "ep").text, d.endpoint);
        } else {
            w.erase(Key(i, "ext").text);
            w.erase(Key(i, "short").text);
            w.erase(Key(i, "ep").text);
        }
    }
    w.commit();
}

void SettingsStore::onFactoryReset()
{
    ESP_LOGW(kTag, "Clearing all roles");
    onPersistRoles(ev::PersistRoles{}); // all records invalid -> keys erased
}

} // namespace sunsim::platform
