#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sunsim {

// Devices are assigned a role by looking up their IEEE address in the
// role manifest (see role_manifest.h), not by join order.
enum class Role : uint8_t {
    WallA  = 0,
    WallB  = 1,
    WallC  = 2,
    Work   = 3,
    Button = 4,
};

inline constexpr std::size_t kRoleCount = 5;

inline constexpr std::array<std::string_view, kRoleCount> kRoleNames = {"WallA", "WallB", "WallC", "Work", "Button"};

[[nodiscard]] constexpr std::string_view roleName(std::size_t index) noexcept
{
    return index < kRoleCount ? kRoleNames[index] : std::string_view{"?"};
}

// Case-sensitive match against kRoleNames, e.g. "WallA".
[[nodiscard]] constexpr std::optional<Role> parseRole(std::string_view name) noexcept
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (kRoleNames[i] == name) {
            return static_cast<Role>(i);
        }
    }
    return std::nullopt;
}

struct DeviceRole {
    bool     valid     = false;
    uint64_t ext_addr   = 0;
    uint16_t short_addr = 0;
    uint8_t  endpoint   = 0;
};

class RoleStore {
public:
    void load();
    void save() const;

    void clear(std::size_t index) noexcept { roles_[index] = {}; }
    void clearAll() noexcept { roles_ = {}; }

    [[nodiscard]] std::optional<std::size_t> findByExtAddr(uint64_t ext_addr) const noexcept;

    [[nodiscard]] DeviceRole &at(std::size_t index) noexcept { return roles_[index]; }
    [[nodiscard]] const DeviceRole &at(std::size_t index) const noexcept { return roles_[index]; }
    [[nodiscard]] DeviceRole &at(Role role) noexcept { return roles_[static_cast<std::size_t>(role)]; }
    [[nodiscard]] const DeviceRole &at(Role role) const noexcept { return roles_[static_cast<std::size_t>(role)]; }

private:
    std::array<DeviceRole, kRoleCount> roles_{};
};

} // namespace sunsim
