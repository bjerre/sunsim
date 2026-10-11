module;

#include "sunsim/ev/log.h"

export module sunsim.core:device_registry;

import std.compat;
import sunsim.ev;

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
    using Manifest = ev::PerRole<uint64_t>; // IEEE address per role; 0 = no entry

    // The build-time manifest (main/role_manifest.json: entries with a role
    // name and an ext_addr) checked against the layout, so changing
    // ev::kLayout needs nothing else changed. An entry naming no role, a
    // role given twice, or one address given to two roles doesn't compile.
    // Names ignore case; a 0 address is no entry.
    template <std::ranges::input_range Entries>
    [[nodiscard]] static consteval Manifest manifestOf(const Entries &entries)
    {
        Manifest manifest{};
        for (const auto &entry : entries) {
            const auto role = ev::parseRole(entry.role);
            if (!role) {
                roleManifestNamesAnUnknownRole();
            } else if (entry.ext_addr != 0) {
                if (manifest[*role] != 0) {
                    roleManifestGivesARoleTwice();
                }
                if (std::ranges::contains(manifest.items, entry.ext_addr)) {
                    roleManifestGivesAnAddressTwice();
                }
                manifest[*role] = entry.ext_addr;
            }
        }
        return manifest;
    }

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
    // Not constexpr: reached while building a manifest, they stop the
    // compile, and the error names them.
    static void roleManifestNamesAnUnknownRole() noexcept {}
    static void roleManifestGivesARoleTwice() noexcept {}
    static void roleManifestGivesAnAddressTwice() noexcept {}

    struct Liveness {
        std::optional<ev::Instant> last_heard{}; // empty: not since boot
        int8_t     rssi      = 0;
        bool       reachable = true;
    };

    [[nodiscard]] ev::DeviceRecord &record(ev::Role role) noexcept { return records_[role]; }
    [[nodiscard]] const ev::DeviceRecord &record(ev::Role role) const noexcept { return records_[role]; }
    [[nodiscard]] Liveness &liveness(ev::Role role) noexcept { return liveness_[role]; }
    [[nodiscard]] const Liveness &liveness(ev::Role role) const noexcept { return liveness_[role]; }

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

    ev::Post                     post_;
    Manifest                     manifest_;
    ev::DeviceRecords            records_{};
    ev::PerRole<Liveness>        liveness_{};
    std::optional<ev::Role>      armed_;

    ev::Timer                    timer_;
    bool                         network_up_ = false; // nothing is sent before
    ev::Inbox<ev::NetworkUp>     network_{timer_};
    ev::Inbox<ev::EndpointFound> endpoints_{timer_};
    ev::Inbox<ev::PingResult>    pings_{timer_};
    ev::PerRole<ev::Flow>        discovery_;  // last, so they're destroyed
    ev::PerRole<ev::Flow>        keep_alive_; // before what they wait on (bulbs only)
};

} // namespace sunsim::core

namespace sunsim::core {

using namespace std::chrono_literals;

namespace {
constexpr const char *kTag = "REGISTRY";

constexpr auto kDiscoveryRetry   = 15s;
constexpr auto kFirstPing        = 15s;  // after the network is up, or pairing
constexpr auto kPingInterval     = 60s;  // reachable bulbs
constexpr auto kUnreachablePing  = 15s;  // until they answer again
constexpr auto kUnreachableAfter = 60s;  // of silence, plus a failed ping
constexpr auto kOnlineWindow     = 150s; // shown as online: a couple of missed pings

const char *name(ev::Role role) noexcept
{
    return ev::roleName(role).data();
}

// A filter for events about `role`.
constexpr auto about(ev::Role role) noexcept
{
    return [role](const auto &e) { return e.role == role; };
}
} // namespace

DeviceRegistry::DeviceRegistry(ev::Post post, Manifest manifest) : post_(std::move(post)), manifest_(manifest) {}

std::optional<ev::Role> DeviceRegistry::roleByExt(uint64_t ext_addr) const noexcept
{
    for (const ev::Role role : ev::kRoles) {
        if (record(role).valid && record(role).ext_addr == ext_addr) {
            return role;
        }
    }
    return std::nullopt;
}

std::optional<ev::Role> DeviceRegistry::roleFromManifest(uint64_t ext_addr) const noexcept
{
    for (const ev::Role role : ev::kRoles) {
        if (ext_addr != 0 && manifest_[role] == ext_addr) {
            return role;
        }
    }
    return std::nullopt;
}

void DeviceRegistry::persist()
{
    post_(ev::PersistRoles{records_});
}

void DeviceRegistry::heard(ev::Role role)
{
    liveness(role).last_heard = timer_.now();
    setReachable(role, true);
}

void DeviceRegistry::setReachable(ev::Role role, bool reachable)
{
    Liveness &l = liveness(role);
    if (l.reachable == reachable) {
        return;
    }
    l.reachable = reachable;
    post_(ev::ReachabilityChanged{role, reachable});
    if (reachable) {
        SUNSIM_LOGI(kTag, "%s is answering again - resuming its light", name(role));
        if (ev::isBulb(role)) {
            post_(ev::RefreshLights{role});
        }
    } else {
        SUNSIM_LOGW(kTag, "%s not answering - pausing its light commands (powered off or out of range?)",
                    name(role));
    }
}

ev::DeviceLink DeviceRegistry::link(ev::Role role) const noexcept
{
    const Liveness &l = liveness(role);
    if (!record(role).valid) {
        return ev::DeviceLink::Unpaired;
    }
    if (l.reachable && l.last_heard && timer_.now() - *l.last_heard <= kOnlineWindow) {
        return ev::DeviceLink::Online;
    }
    return ev::DeviceLink::Silent;
}

void DeviceRegistry::assign(ev::Role role, uint64_t ext_addr, uint16_t short_addr)
{
    record(role) = {.valid = true, .ext_addr = ext_addr, .short_addr = short_addr, .endpoint = 0};
    Liveness &l  = liveness(role);
    l            = {.reachable = l.reachable}; // heard() reports it reachable if it wasn't
    persist();
    heard(role);
    post_(ev::RoleAssigned{role, record(role)});
    discovery_[role] = findEndpoint(role);
    if (ev::isBulb(role)) {
        keep_alive_[role] = keepAlive(role);
    }
}

void DeviceRegistry::clear(ev::Role role)
{
    discovery_[role]  = {};
    keep_alive_[role] = {};
    record(role)   = {};
    liveness(role) = {};
    post_(ev::RoleCleared{role});
}

void DeviceRegistry::onRolesLoaded(const ev::RolesLoaded &e)
{
    records_ = e.records;
    for (const ev::Role role : ev::kRoles) {
        if (!record(role).valid) {
            continue;
        }
        post_(ev::RoleAssigned{role, record(role)});
        if (record(role).endpoint == 0) {
            discovery_[role] = findEndpoint(role);
        }
        if (ev::isBulb(role)) {
            keep_alive_[role] = keepAlive(role);
        }
    }
}

void DeviceRegistry::onNetworkUp(const ev::NetworkUp &e)
{
    network_up_ = true;
    network_.deliver(e, e.at);
}

void DeviceRegistry::onAnnounced(const ev::DeviceAnnounced &e)
{
    if (const auto existing = roleByExt(e.ext_addr)) {
        ev::DeviceRecord &d = record(*existing);
        if (d.short_addr != e.short_addr) {
            d.short_addr = e.short_addr;
            persist();
        }
        SUNSIM_LOGI(kTag, "Known device rejoined: %s short=0x%04x", name(*existing), e.short_addr);
        heard(*existing);
        post_(ev::RoleAssigned{*existing, d});
        if (!ev::isBulb(*existing) || d.endpoint == 0) {
            discovery_[*existing] = findEndpoint(*existing); // a button is re-bound
        } else {
            post_(ev::RefreshLights{*existing});
        }
        return;
    }

    if (armed_) {
        const ev::Role role = *armed_;
        armed_.reset();
        SUNSIM_LOGI(kTag, "Paired ext=0x%016llx short=0x%04x -> %s", static_cast<unsigned long long>(e.ext_addr),
                    e.short_addr, name(role));
        assign(role, e.ext_addr, e.short_addr);
        return;
    }

    const auto role = roleFromManifest(e.ext_addr);
    if (!role) {
        SUNSIM_LOGW(kTag,
                    "Ignoring unknown device 0x%04x (ext=0x%016llx): not in role manifest (use `pair <role>` to "
                    "claim it)",
                    e.short_addr, static_cast<unsigned long long>(e.ext_addr));
        return;
    }
    if (record(*role).valid) {
        SUNSIM_LOGW(kTag, "Ignoring device 0x%04x: %s already assigned to a different device", e.short_addr,
                    name(*role));
        return;
    }
    SUNSIM_LOGI(kTag, "New device joined -> %s (from role manifest)", name(*role));
    assign(*role, e.ext_addr, e.short_addr);
}

void DeviceRegistry::onEndpointFound(const ev::EndpointFound &e)
{
    endpoints_.deliver(e);
}

ev::Flow DeviceRegistry::findEndpoint(ev::Role role)
{
    if (!network_up_) {
        co_await network_.next(); // roles restored at boot: ask once there's a network
    }
    for (;;) {
        const ev::Instant retry_at = timer_.now() + kDiscoveryRetry;
        post_(ev::Discover{role, record(role).short_addr});
        // A reply without one (the adapter logged why) waits out the interval.
        while (const auto found = co_await endpoints_.next(about(role), retry_at)) {
            if (found->endpoint != 0) {
                co_return endpointFound(role, found->endpoint);
            }
        }
        SUNSIM_LOGI(kTag, "Retrying endpoint discovery for %s (short=0x%04x)", name(role), record(role).short_addr);
    }
}

void DeviceRegistry::endpointFound(ev::Role role, uint8_t endpoint)
{
    ev::DeviceRecord &d = record(role);
    d.endpoint          = endpoint;
    persist();
    post_(ev::RoleAssigned{role, d});
    if (!ev::isBulb(role)) {
        SUNSIM_LOGI(kTag, "%s endpoint = %d, binding to local receiver", name(role), d.endpoint);
        post_(ev::Bind{role, d.ext_addr, d.short_addr, d.endpoint});
    } else {
        SUNSIM_LOGI(kTag, "%s (bulb) endpoint = %d", name(role), d.endpoint);
        post_(ev::RefreshLights{role});
    }
}

void DeviceRegistry::onBindResult(const ev::BindResult &e)
{
    if (e.ok) {
        SUNSIM_LOGI(kTag, "Bound %s to local receiver endpoint", name(e.role));
    } else {
        SUNSIM_LOGE(kTag, "Failed to bind %s", name(e.role));
    }
}

void DeviceRegistry::onPingResult(const ev::PingResult &e)
{
    pings_.deliver(e);
}

// A button is a sleepy end device that won't answer pings; it counts as
// heard when it's pressed instead.
ev::Flow DeviceRegistry::keepAlive(ev::Role bulb)
{
    if (!network_up_) {
        co_await network_.next();
    }
    co_await timer_.after(kFirstPing);
    for (;;) {
        const ev::Instant sent = timer_.now();
        post_(ev::Ping{bulb, record(bulb).short_addr});
        const auto reply = co_await pings_.next(about(bulb), sent + kUnreachablePing);
        if (reply && reply->ok) {
            heard(bulb);
        } else {
            // A lost ping alone isn't enough (a busy network drops some); it
            // must also have been silent - no acks - for a while.
            const Liveness &l = liveness(bulb);
            if (!l.last_heard || timer_.now() - *l.last_heard > kUnreachableAfter) {
                SUNSIM_LOGD(kTag, "Ping to %s failed (err=0x%x status=0x%x)", name(bulb), reply ? reply->error : -1,
                            reply ? reply->status : -1);
                setReachable(bulb, false);
            }
        }
        co_await timer_.until(sent + (liveness(bulb).reachable ? kPingInterval : kUnreachablePing));
    }
}

void DeviceRegistry::onAck(const ev::AckReceived &e)
{
    for (const ev::Role role : ev::kRoles) {
        if (record(role).valid && record(role).short_addr == e.short_addr) {
            heard(role);
            liveness(role).rssi = e.rssi;
        }
    }
}

void DeviceRegistry::onButtonPressed(const ev::ButtonPressed &e)
{
    if (e.source != ev::Source::Zigbee) {
        return;
    }
    for (const ev::Role button : ev::members(ev::Group::Button)) { // a press doesn't say which
        if (record(button).valid) {
            heard(button);
        }
    }
}

void DeviceRegistry::onPairRequested(const ev::PairRequested &e)
{
    if (record(e.role).valid) {
        SUNSIM_LOGW(kTag, "%s is already assigned; the next device to join will replace it", name(e.role));
    }
    armed_ = e.role;
    post_(ev::OpenNetwork{});
    SUNSIM_LOGI(kTag,
                "Armed %s: factory-reset / power on the device now. The next device that joins and doesn't already "
                "hold a role becomes %s.",
                name(e.role), name(e.role));
}

void DeviceRegistry::onPairCancelled(const ev::PairCancelled &)
{
    armed_.reset();
    SUNSIM_LOGI(kTag, "Pairing cancelled");
}

void DeviceRegistry::onUnpairRequested(const ev::UnpairRequested &e)
{
    for (const ev::Role role : ev::kRoles) {
        if (ev::selects(e.roles, role)) {
            clear(role);
            if (!std::holds_alternative<ev::AllRoles>(e.roles)) {
                SUNSIM_LOGI(kTag, "%s cleared", name(role));
            }
        }
    }
    if (std::holds_alternative<ev::AllRoles>(e.roles)) {
        SUNSIM_LOGI(kTag, "All roles cleared");
    }
    persist();
}

void DeviceRegistry::onTick(const ev::Tick &e)
{
    timer_.tick(e.at);
    publishDevices();
}

void DeviceRegistry::publishDevices()
{
    ev::DevicesUpdated u{.devices = {}, .records = records_, .armed = armed_};
    for (const ev::Role role : ev::kRoles) {
        const Liveness &l = liveness(role);
        u.devices[role] = {.link         = link(role),
                           .paired       = record(role).valid,
                           .has_endpoint = record(role).endpoint != 0,
                           .last_heard   = l.last_heard,
                           .rssi         = l.rssi,
                           .reachable    = l.reachable};
    }
    post_(u);
}

} // namespace sunsim::core
