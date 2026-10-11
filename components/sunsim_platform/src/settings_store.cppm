module;

#include "esp_log.h"
#include "nvs.h"

export module sunsim.platform:settings_store;

import std.compat;
import sunsim.ev;

export namespace sunsim::platform {

// Everything sunsim persists, in NVS namespace "sunsim", one key per field
// so the format survives firmware changes (a new field just reads as its
// default):
//   roles     "<role>.ext" / ".short" / ".ep"   (a role is assigned iff .ext exists)
//   program   "act.start" "act.end" "act.fade" "act.days" "work.start"
//             "work.end" "sun.shift" "sun.amber" "sun.sweep" "sync.start"
//             "sync.end" "sky.on" "sky.dim" "sky.cool"  (hours stored as minutes)
//   intensity "<role>.peak" (percent), "<role>.min" (minimum level)
//   "layout"  which role layout the keys follow (see migrateLayout())
// Wi-Fi credentials are kept separately (:wifi).
class SettingsStore {
public:
    // `defaults`: the program settings before anything was saved (location
    // comes from menuconfig).
    SettingsStore(ev::Post post, ev::ProgramConfig defaults);

    // Posts RolesLoaded and ConfigChanged with what's stored.
    void loadAll();

    void onSetConfig(const ev::SetConfig &e);
    void onSetIntensity(const ev::SetIntensity &e);
    void onSetMinLevel(const ev::SetMinLevel &e);
    void onPersistRoles(const ev::PersistRoles &e);
    void onFactoryReset();

private:
    void publish();

    ev::Post          post_;
    ev::ProgramConfig config_;
    ev::Peaks         peaks_      = ev::kDefaultPeaks;
    ev::MinLevels     min_levels_ = ev::kDefaultMinLevels;
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
constexpr const char *kSweepKey     = "sun.sweep";
constexpr const char *kCloudsKey    = "sky.on";
constexpr const char *kCloudDimKey  = "sky.dim";
constexpr const char *kCloudCoolKey = "sky.cool";
constexpr const char *kSyncStartKey = "sync.start";
constexpr const char *kSyncEndKey   = "sync.end";

uint16_t toMinutes(double hours) noexcept
{
    return static_cast<uint16_t>(std::lround(hours * 60.0));
}

// "<role>.<field>", at most 15 chars (NVS key limit).
struct Key {
    char text[16];
    Key(std::string_view role_name, const char *field)
    {
        std::snprintf(text, sizeof(text), "%.*s.%s", static_cast<int>(role_name.size()), role_name.data(), field);
    }
    Key(ev::Role role, const char *field) : Key(ev::roleName(role), field) {}
};
static_assert(
    [] {
        std::size_t longest = 0;
        for (const ev::Role role : ev::kRoles) {
            longest = std::max(longest, ev::roleName(role).size());
        }
        return longest + std::string_view{".short"}.size() <= 15;
    }(),
    "role names must fit NVS keys");

// The role layout the stored keys follow. Missing: the three-light wall,
// whose WallA/B/C sat at the east end, middle and west end.
constexpr const char *kLayoutKey     = "layout";
constexpr uint8_t     kFiveLightWall = 1;

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

// Moves one key's value to another key; false if there was none.
template <class T, auto get, auto set> bool moveKey(nvs_handle_t h, const Key &from, const Key &to)
{
    T value{};
    if (get(h, from.text, &value) != ESP_OK || set(h, to.text, value) != ESP_OK) {
        return false;
    }
    nvs_erase_key(h, from.text);
    return true;
}

// Moves everything stored for a role to another role's name.
bool moveRole(nvs_handle_t h, std::string_view from, std::string_view to)
{
    bool moved = moveKey<uint64_t, nvs_get_u64, nvs_set_u64>(h, Key(from, "ext"), Key(to, "ext"));
    moved |= moveKey<uint16_t, nvs_get_u16, nvs_set_u16>(h, Key(from, "short"), Key(to, "short"));
    moved |= moveKey<uint8_t, nvs_get_u8, nvs_set_u8>(h, Key(from, "ep"), Key(to, "ep"));
    moved |= moveKey<uint8_t, nvs_get_u8, nvs_set_u8>(h, Key(from, "peak"), Key(to, "peak"));
    moved |= moveKey<uint8_t, nvs_get_u8, nvs_set_u8>(h, Key(from, "min"), Key(to, "min"));
    return moved;
}

// The three-light wall's WallA/B/C (east end, middle, west end) are the
// five-light wall's WallA/C/E: the same places. Renames their keys, once.
void migrateLayout()
{
    Nvs nvs{NVS_READWRITE};
    if (!nvs.ok()) {
        ESP_LOGE(kTag, "Can't open NVS to check the role layout: %s", esp_err_to_name(nvs.error()));
        return;
    }
    const nvs_handle_t h      = nvs.get();
    uint8_t            layout = 0;
    if (nvs_get_u8(h, kLayoutKey, &layout) == ESP_OK && layout >= kFiveLightWall) {
        return;
    }
    const bool moved_c = moveRole(h, "WallC", "WallE"); // first, so WallC is free for WallB
    const bool moved_b = moveRole(h, "WallB", "WallC");
    nvs_set_u8(h, kLayoutKey, kFiveLightWall);
    if (const esp_err_t err = nvs_commit(h); err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save the role layout: %s", esp_err_to_name(err));
    } else if (moved_b || moved_c) {
        ESP_LOGW(kTag, "Five-light wall: stored WallB is now WallC, WallC is now WallE (same places on the wall)");
    }
}
} // namespace

SettingsStore::SettingsStore(ev::Post post, ev::ProgramConfig defaults) : post_(std::move(post)), config_(defaults) {}

void SettingsStore::publish()
{
    post_(ev::ConfigChanged{.config = config_, .peaks = peaks_, .min_levels = min_levels_});
}

void SettingsStore::loadAll()
{
    migrateLayout();

    ev::RolesLoaded roles{};

    Nvs nvs{NVS_READONLY};
    if (!nvs.ok()) {
        ESP_LOGI(kTag, "Nothing stored yet");
    } else {
        const nvs_handle_t h = nvs.get();

        for (const ev::Role role : ev::kRoles) {
            ev::DeviceRecord &d = roles.records[role];
            if (nvs_get_u64(h, Key(role, "ext").text, &d.ext_addr) != ESP_OK) {
                continue;
            }
            d.valid = true;
            nvs_get_u16(h, Key(role, "short").text, &d.short_addr); // 0 if missing; refreshed on next announce
            nvs_get_u8(h, Key(role, "ep").text, &d.endpoint);       // 0 if missing; triggers rediscovery
            ESP_LOGI(kTag, "Restored %s: ext=0x%016llx short=0x%04x ep=%d", ev::roleName(role).data(),
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
        if (nvs_get_u16(h, kSyncStartKey, &minutes) == ESP_OK) {
            config_.sync_start_hour = minutes / 60.0;
        }
        if (nvs_get_u16(h, kSyncEndKey, &minutes) == ESP_OK) {
            config_.sync_end_hour = minutes / 60.0;
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
        if (nvs_get_u8(h, kSweepKey, &byte) == ESP_OK && byte <= ev::kMaxAmberSweepDeg) {
            config_.amber_sweep_deg = byte;
        }
        if (nvs_get_u8(h, kCloudsKey, &byte) == ESP_OK) {
            config_.clouds_enabled = byte != 0;
        }
        if (nvs_get_u8(h, kCloudDimKey, &byte) == ESP_OK && byte <= 100) {
            config_.cloud_dim_percent = byte;
        }
        if (nvs_get_u8(h, kCloudCoolKey, &byte) == ESP_OK && byte <= 100) {
            config_.cloud_cool_percent = byte;
        }
        for (const ev::Role role : ev::kRoles) {
            if (nvs_get_u8(h, Key(role, "peak").text, &byte) == ESP_OK && byte >= 1 && byte <= 100) {
                peaks_[role] = byte;
            }
            if (nvs_get_u8(h, Key(role, "min").text, &byte) == ESP_OK && byte >= 1 && byte <= ev::kMaxLevel) {
                min_levels_[role] = byte;
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
    w.u16(kSyncStartKey, toMinutes(config_.sync_start_hour));
    w.u16(kSyncEndKey, toMinutes(config_.sync_end_hour));
    w.u8(kSunShiftKey, config_.sun_shift_percent);
    w.u8(kAmberKey, config_.amber_enabled ? 1 : 0);
    w.u8(kSweepKey, config_.amber_sweep_deg);
    w.u8(kCloudsKey, config_.clouds_enabled ? 1 : 0);
    w.u8(kCloudDimKey, config_.cloud_dim_percent);
    w.u8(kCloudCoolKey, config_.cloud_cool_percent);
    w.commit();
    publish();
}

void SettingsStore::onSetIntensity(const ev::SetIntensity &e)
{
    const uint8_t percent = e.percent < 1 ? 1 : (e.percent > 100 ? 100 : e.percent);

    Nvs       nvs{NVS_READWRITE};
    NvsWriter w{nvs, "intensity"};
    for (const ev::Role role : ev::bulbs()) {
        if (ev::selects(e.lights, role)) {
            peaks_[role] = percent;
            w.u8(Key(role, "peak").text, percent);
        }
    }
    w.commit();
    publish();
}

void SettingsStore::onSetMinLevel(const ev::SetMinLevel &e)
{
    const uint8_t level = std::clamp<uint8_t>(e.level, 1, ev::kMaxLevel);

    Nvs       nvs{NVS_READWRITE};
    NvsWriter w{nvs, "minimum level"};
    for (const ev::Role role : ev::bulbs()) {
        if (ev::selects(e.lights, role)) {
            min_levels_[role] = level;
            w.u8(Key(role, "min").text, level);
        }
    }
    w.commit();
    publish();
}

void SettingsStore::onPersistRoles(const ev::PersistRoles &e)
{
    Nvs       nvs{NVS_READWRITE};
    NvsWriter w{nvs, "roles"};
    w.erase(kLegacyBlobKey);
    for (const ev::Role role : ev::kRoles) {
        const ev::DeviceRecord &d = e.records[role];
        if (d.valid) {
            w.u64(Key(role, "ext").text, d.ext_addr);
            w.u16(Key(role, "short").text, d.short_addr);
            w.u8(Key(role, "ep").text, d.endpoint);
        } else {
            w.erase(Key(role, "ext").text);
            w.erase(Key(role, "short").text);
            w.erase(Key(role, "ep").text);
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
