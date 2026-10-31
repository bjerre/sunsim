module;

#include "nvs.h"

export module sunsim.platform:installation_profile;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::platform {

// A replaceable runtime profile, transferred over the USB console and kept in
// NVS. It owns the builder so every ZoneSpec name remains valid for the app's
// lifetime.
class InstallationProfile {
public:
    static constexpr std::size_t kMaxJsonBytes = 3072;

    InstallationProfile();

    [[nodiscard]] const ev::Installation &installation() const noexcept;
    [[nodiscard]] bool beginUpload(std::size_t size) noexcept;
    [[nodiscard]] bool appendUpload(std::size_t offset, std::string_view bytes) noexcept;
    [[nodiscard]] bool saveUpload();
    [[nodiscard]] bool clearSaved();

private:
    [[nodiscard]] bool parse(std::string_view json, ev::InstallationProfileBuilder &builder) const;
    [[nodiscard]] bool load();

    ev::InstallationProfileBuilder active_;
    ev::InstallationProfileBuilder candidate_;
    std::array<char, kMaxJsonBytes + 1> stored_json_{};
    std::array<char, kMaxJsonBytes + 1> upload_json_{};
    std::size_t upload_size_ = 0;
    std::size_t received_ = 0;
    bool uploading_ = false;
};

} // namespace sunsim::platform

namespace sunsim::platform {

namespace {
constexpr const char *kTag = "PROFILE";
constexpr const char *kNamespace = "sunsim";
constexpr const char *kProfileKey = "install";
} // namespace

InstallationProfile::InstallationProfile()
{
    (void)load();
}

const ev::Installation &InstallationProfile::installation() const noexcept
{
    return active_.ready() ? active_.installation() : ev::kDefaultInstallation;
}

bool InstallationProfile::parse(std::string_view json, ev::InstallationProfileBuilder &builder) const
{
    if (json.empty() || json.size() > kMaxJsonBytes || json.find('\0') != std::string_view::npos) {
        return false;
    }
    return builder.parseJson(json);
}

bool InstallationProfile::load()
{
    nvs_handle_t handle{};
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    std::size_t size = 0;
    esp_err_t error = nvs_get_str(handle, kProfileKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return false;
    }
    if (error != ESP_OK || size == 0 || size > stored_json_.size()) {
        ev::log::warn(kTag, "Saved profile is too large or unreadable; using built-in installation");
        nvs_close(handle);
        return false;
    }
    error = nvs_get_str(handle, kProfileKey, stored_json_.data(), &size);
    nvs_close(handle);
    if (error != ESP_OK || !parse(std::string_view{stored_json_.data(), size - 1}, active_)) {
        ev::log::warn(kTag, "Saved profile is invalid; using built-in installation");
        return false;
    }
    ev::log::info(kTag, "Loaded runtime installation profile");
    return true;
}

bool InstallationProfile::beginUpload(std::size_t size) noexcept
{
    if (size == 0 || size > kMaxJsonBytes) {
        return false;
    }
    upload_json_.fill('\0');
    upload_size_ = size;
    received_ = 0;
    uploading_ = true;
    return true;
}

bool InstallationProfile::appendUpload(std::size_t offset, std::string_view bytes) noexcept
{
    if (!uploading_ || offset != received_ || bytes.empty() || bytes.size() > upload_size_ - received_ ||
        bytes.find('\0') != std::string_view::npos) {
        return false;
    }
    std::ranges::copy(bytes, upload_json_.begin() + static_cast<std::ptrdiff_t>(received_));
    received_ += bytes.size();
    return true;
}

bool InstallationProfile::saveUpload()
{
    if (!uploading_ || received_ != upload_size_) {
        return false;
    }
    const std::string_view json{upload_json_.data(), upload_size_};
    if (!parse(json, candidate_)) {
        uploading_ = false;
        ev::log::warn(kTag, "Rejected invalid runtime profile; saved profile is unchanged");
        return false;
    }

    nvs_handle_t handle{};
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, kProfileKey, upload_json_.data());
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    uploading_ = false;
    if (error != ESP_OK) {
        ev::log::error(kTag, "Failed to save profile: {}", esp_err_to_name(error));
        return false;
    }
    return true;
}

bool InstallationProfile::clearSaved()
{
    nvs_handle_t handle{};
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return false;
    }
    error = nvs_erase_key(handle, kProfileKey);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error == ESP_OK;
}

} // namespace sunsim::platform
