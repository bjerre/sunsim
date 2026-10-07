#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "sunsim/ev/events.h"

namespace sunsim::core {

// Which device holds which role, and whether it's alive.
//  - Pairing: a device announcing itself while a role is armed (console
//    `pair`) gets that role, unless it already holds one; otherwise the
//    build-time role manifest (IEEE address -> role) decides; anything else
//    is ignored.
//  - Discovery: finds each device's endpoint (Discover/EndpointFound),
//    retried every 15 s until it's known; the button is then bound (Bind).
//  - Liveness: every bulb is pinged every 60 s; acks, ping replies,
//    announces and button presses count as "heard".
// Persists through PersistRoles; never touches storage or the radio itself.
class DeviceRegistry {
public:
    using Manifest = std::array<uint64_t, ev::kRoleCount>; // 0 = no entry

    DeviceRegistry(ev::Post post, Manifest manifest);

    void onRolesLoaded(const ev::RolesLoaded &e);
    void onAnnounced(const ev::DeviceAnnounced &e);
    void onEndpointFound(const ev::EndpointFound &e);
    void onBindResult(const ev::BindResult &e);
    void onPingResult(const ev::PingResult &e);
    void onAck(const ev::AckReceived &e);
    void onButtonPressed(const ev::ButtonPressed &e);
    void onPairRequested(const ev::PairRequested &e);
    void onPairCancelled(const ev::PairCancelled &e);
    void onUnpairRequested(const ev::UnpairRequested &e);
    void onTick(const ev::Tick &e);

private:
    struct Liveness {
        int64_t last_heard_us = 0;
        int8_t  rssi          = 0;
        bool    ping_ok       = true; // last ping's outcome, for logging changes only
    };

    [[nodiscard]] std::optional<ev::Role> roleByExt(uint64_t ext_addr) const noexcept;
    [[nodiscard]] std::optional<ev::Role> roleFromManifest(uint64_t ext_addr) const noexcept;
    void assign(ev::Role role, uint64_t ext_addr, uint16_t short_addr);
    void clear(ev::Role role);
    void heard(ev::Role role);
    void persist();
    void discover(ev::Role role);

    ev::Post                                post_;
    Manifest                                manifest_;
    ev::DeviceRecords                       records_{};
    std::array<Liveness, ev::kRoleCount>    liveness_{};
    std::optional<ev::Role>                 armed_;
    int64_t                                 now_us_  = 0;
    uint32_t                                seconds_ = 0;
};

} // namespace sunsim::core
