module;

#include <cstdint>

#include "sunsim/ev/events.h"
#include "sunsim/ev/flow.h"

export module sunsim.core:device_registry;

import std;

export namespace sunsim::core {

// Which device holds which role, and whether it's alive.
//  - Pairing: a device announcing itself while a role is armed (console
//    `pair`) gets that role, unless it already holds one; otherwise the
//    build-time role manifest (IEEE address -> role) decides; anything else
//    is ignored.
//  - Discovery (a flow per role): finds the device's endpoint
//    (Discover/EndpointFound), retried every 15 s until it's known; the
//    button is then bound (Bind).
//  - Liveness (a flow per bulb): acks, ping replies, announces and button
//    presses count as "heard". Bulbs are pinged 15 s after the network is up
//    (or after pairing), then every
//    60 s. A failed or unanswered ping with nothing heard for a minute marks
//    a bulb unreachable (ReachabilityChanged): its light commands pause -
//    sending to a missing device ties up the stack's buffers with retries
//    and starves everything else - and it's pinged every 15 s until it
//    answers anything, when it's marked reachable again and its light
//    re-applied. Heard within 150 s (and reachable) shows as Online in
//    DevicesUpdated, otherwise Silent.
// Persists through PersistRoles; never touches storage or the radio itself.
class DeviceRegistry {
public:
    using Manifest = std::array<uint64_t, ev::kRoleCount>; // 0 = no entry

    DeviceRegistry(ev::Post post, Manifest manifest);

    void onRolesLoaded(const ev::RolesLoaded &e);
    void onNetworkUp(const ev::NetworkUp &e);
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
        std::optional<ev::Instant> last_heard{}; // empty: not since boot
        int8_t     rssi      = 0;
        bool       reachable = true;
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
    void endpointFound(ev::Role role, uint8_t endpoint);
    [[nodiscard]] ev::DeviceLink link(ev::Role role) const noexcept;
    void publishDevices();

    // Flows; reassigning a slot restarts it, an empty Flow{} stops it.
    ev::Flow findEndpoint(ev::Role role);
    ev::Flow keepAlive(ev::Role bulb);

    ev::Post                             post_;
    Manifest                             manifest_;
    ev::DeviceRecords                    records_{};
    std::array<Liveness, ev::kRoleCount> liveness_{};
    std::optional<ev::Role>              armed_;

    ev::Timer                            timer_;
    bool                                 network_up_ = false; // nothing is sent before
    ev::Inbox<ev::NetworkUp>             network_{timer_};
    ev::Inbox<ev::EndpointFound>         endpoints_{timer_};
    ev::Inbox<ev::PingResult>            pings_{timer_};
    std::array<ev::Flow, ev::kRoleCount> discovery_;  // last, so they're destroyed
    std::array<ev::Flow, ev::kBulbCount> keep_alive_; // before what they wait on
};

} // namespace sunsim::core
