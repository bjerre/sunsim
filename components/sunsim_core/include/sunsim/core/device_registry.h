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
//  - Liveness: acks, ping replies, announces and button presses count as
//    "heard". Bulbs are pinged 15 s after boot, then every 60 s. A failed
//    ping with nothing heard for a minute marks a bulb unreachable
//    (ReachabilityChanged): its light commands pause - sending to a missing
//    device ties up the stack's buffers with retries and starves everything
//    else - and it's pinged every 15 s until it answers anything, when it's
//    marked reachable again and its light re-applied. Heard within 150 s
//    (and reachable) shows as Online in DevicesUpdated, otherwise Silent.
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
        int64_t  last_heard_us = 0;
        int8_t   rssi          = 0;
        bool     reachable     = true;
        uint32_t next_ping_s   = 0; // 0 = first ping due kFirstPingSeconds after boot
    };

    [[nodiscard]] ev::DeviceRecord &record(ev::Role role) noexcept { return records_[ev::indexOf(role)]; }
    [[nodiscard]] const ev::DeviceRecord &record(ev::Role role) const noexcept { return records_[ev::indexOf(role)]; }
    [[nodiscard]] Liveness &liveness(ev::Role role) noexcept { return liveness_[ev::indexOf(role)]; }
    [[nodiscard]] const Liveness &liveness(ev::Role role) const noexcept { return liveness_[ev::indexOf(role)]; }

    [[nodiscard]] std::optional<ev::Role> roleByExt(uint64_t ext_addr) const noexcept;
    [[nodiscard]] std::optional<ev::Role> roleFromManifest(uint64_t ext_addr) const noexcept;
    void assign(ev::Role role, uint64_t ext_addr, uint16_t short_addr);
    void clear(ev::Role role);
    void heard(ev::Role role);
    void setReachable(ev::Role role, bool reachable);
    void persist();
    void discover(ev::Role role);
    [[nodiscard]] ev::DeviceLink link(ev::Role role) const noexcept;

    // The three things a Tick does.
    void retryDiscovery();
    void pingDue();
    void publishDevices();

    ev::Post                                post_;
    Manifest                                manifest_;
    ev::DeviceRecords                       records_{};
    std::array<Liveness, ev::kRoleCount>    liveness_{};
    std::optional<ev::Role>                 armed_;
    int64_t                                 now_us_  = 0;
    uint32_t                                seconds_ = 0;
};

} // namespace sunsim::core
