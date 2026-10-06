#include "roles_store.h"

#include <cstdio>

#include "esp_log.h"
#include "nvs.h"

namespace sunsim {

namespace {
constexpr const char *kTag       = "ROLES";
constexpr const char *kNamespace = "sunsim";

// Pre-versioning format: the whole roles array as one raw-struct blob, which
// silently failed to load whenever DeviceRole or kRoleCount changed. Only
// erased now, never read.
constexpr const char *kLegacyBlobKey = "roles";

// Each role field is its own NVS key named "<role>.<field>" (e.g. "WallA.ext",
// at most 15 chars), so the stored format survives firmware changes: a new
// role or field just reads as absent, and enum reordering can't mix roles up
// since keys go by name. A role is assigned iff its ".ext" key exists.
// Adding a field: give it a new key and a sane default when it's missing.
struct FieldKeys {
    char ext[16];
    char short_addr[16];
    char endpoint[16];
};

FieldKeys keysFor(std::size_t index)
{
    const auto name = roleName(index);
    const int  len  = static_cast<int>(name.size());
    FieldKeys  k{};
    std::snprintf(k.ext, sizeof(k.ext), "%.*s.ext", len, name.data());
    std::snprintf(k.short_addr, sizeof(k.short_addr), "%.*s.short", len, name.data());
    std::snprintf(k.endpoint, sizeof(k.endpoint), "%.*s.ep", len, name.data());
    return k;
}
} // namespace

void RoleStore::load()
{
    roles_ = {};

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(kTag, "No stored roles yet");
        return;
    }

    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const FieldKeys k = keysFor(i);
        DeviceRole     &d = roles_[i];
        if (nvs_get_u64(handle, k.ext, &d.ext_addr) != ESP_OK) {
            continue;
        }
        d.valid = true;
        nvs_get_u16(handle, k.short_addr, &d.short_addr); // stays 0 if missing; refreshed on next announce
        nvs_get_u8(handle, k.endpoint, &d.endpoint);      // stays 0 if missing; triggers rediscovery
        ESP_LOGI(kTag, "Restored %s: ext=0x%016llx short=0x%04x ep=%d", roleName(i).data(),
                 static_cast<unsigned long long>(d.ext_addr), d.short_addr, d.endpoint);
    }
    nvs_close(handle);
}

void RoleStore::save() const
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to open NVS for writing roles");
        return;
    }

    nvs_erase_key(handle, kLegacyBlobKey); // ESP_ERR_NVS_NOT_FOUND once gone; harmless

    esp_err_t err = ESP_OK;
    for (std::size_t i = 0; i < kRoleCount && err == ESP_OK; ++i) {
        const FieldKeys   k = keysFor(i);
        const DeviceRole &d = roles_[i];
        if (d.valid) {
            err = nvs_set_u64(handle, k.ext, d.ext_addr);
            if (err == ESP_OK) {
                err = nvs_set_u16(handle, k.short_addr, d.short_addr);
            }
            if (err == ESP_OK) {
                err = nvs_set_u8(handle, k.endpoint, d.endpoint);
            }
        } else {
            nvs_erase_key(handle, k.ext);
            nvs_erase_key(handle, k.short_addr);
            nvs_erase_key(handle, k.endpoint);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save roles: %s", esp_err_to_name(err));
    }
}

std::optional<std::size_t> RoleStore::findByExtAddr(uint64_t ext_addr) const noexcept
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (roles_[i].valid && roles_[i].ext_addr == ext_addr) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace sunsim
