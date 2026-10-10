#include "sunsim/core/device_registry.h"

#include "sunsim/ev/log.h"

namespace sunsim::core {

namespace {
constexpr const char *kTag = "REGISTRY";

constexpr uint32_t kDiscoveryRetrySeconds   = 15;
constexpr uint32_t kFirstPingSeconds        = 15;
constexpr uint32_t kPingSeconds             = 60; // reachable bulbs
constexpr uint32_t kUnreachablePingSeconds  = 15; // until they answer again
constexpr int64_t  kUnreachableSilenceUs    = 60LL * 1000 * 1000;
// Shown as online if heard within this: a couple of missed 60 s pings.
constexpr int64_t  kOnlineWindowUs          = 150LL * 1000 * 1000;

const char *name(ev::Role role) noexcept
{
    return ev::roleName(role).data();
}
} // namespace

DeviceRegistry::DeviceRegistry(ev::Post post, Manifest manifest) : post_(std::move(post)), manifest_(manifest) {}

std::optional<ev::Role> DeviceRegistry::roleByExt(uint64_t ext_addr) const noexcept
{
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        if (records_[i].valid && records_[i].ext_addr == ext_addr) {
            return static_cast<ev::Role>(i);
        }
    }
    return std::nullopt;
}

std::optional<ev::Role> DeviceRegistry::roleFromManifest(uint64_t ext_addr) const noexcept
{
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        if (ext_addr != 0 && manifest_[i] == ext_addr) {
            return static_cast<ev::Role>(i);
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
    liveness_[ev::indexOf(role)].last_heard_us = now_us_ > 0 ? now_us_ : 1;
    setReachable(role, true);
}

void DeviceRegistry::setReachable(ev::Role role, bool reachable)
{
    Liveness &l = liveness_[ev::indexOf(role)];
    if (l.reachable == reachable) {
        return;
    }
    l.reachable = reachable;
    post_(ev::ReachabilityChanged{role, reachable});
    if (reachable) {
        SUNSIM_LOGI(kTag, "%s is answering again - resuming its light", name(role));
        l.next_ping_s = seconds_ + kPingSeconds;
        if (ev::isBulb(role)) {
            post_(ev::RefreshLights{role});
        }
    } else {
        SUNSIM_LOGW(kTag, "%s not answering - pausing its light commands (powered off or out of range?)",
                    name(role));
        l.next_ping_s = seconds_ + kUnreachablePingSeconds;
    }
}

ev::DeviceLink DeviceRegistry::link(ev::Role role) const noexcept
{
    const Liveness &l = liveness_[ev::indexOf(role)];
    if (!records_[ev::indexOf(role)].valid) {
        return ev::DeviceLink::Unpaired;
    }
    if (l.reachable && l.last_heard_us != 0 && now_us_ - l.last_heard_us <= kOnlineWindowUs) {
        return ev::DeviceLink::Online;
    }
    return ev::DeviceLink::Silent;
}

void DeviceRegistry::discover(ev::Role role)
{
    post_(ev::Discover{role, records_[ev::indexOf(role)].short_addr});
}

void DeviceRegistry::assign(ev::Role role, uint64_t ext_addr, uint16_t short_addr)
{
    records_[ev::indexOf(role)] = {.valid = true, .ext_addr = ext_addr, .short_addr = short_addr, .endpoint = 0};
    Liveness &l = liveness_[ev::indexOf(role)];
    l           = {.reachable = l.reachable}; // heard() reports it reachable if it wasn't
    persist();
    heard(role);
    post_(ev::RoleAssigned{role, records_[ev::indexOf(role)]});
    discover(role);
}

void DeviceRegistry::clear(ev::Role role)
{
    records_[ev::indexOf(role)] = {};
    liveness_[ev::indexOf(role)] = {};
    post_(ev::RoleCleared{role});
}

void DeviceRegistry::onRolesLoaded(const ev::RolesLoaded &e)
{
    records_ = e.records;
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        if (records_[i].valid) {
            post_(ev::RoleAssigned{static_cast<ev::Role>(i), records_[i]});
        }
    }
}

void DeviceRegistry::onAnnounced(const ev::DeviceAnnounced &e)
{
    if (const auto existing = roleByExt(e.ext_addr)) {
        ev::DeviceRecord &d = records_[ev::indexOf(*existing)];
        if (d.short_addr != e.short_addr) {
            d.short_addr = e.short_addr;
            persist();
        }
        SUNSIM_LOGI(kTag, "Known device rejoined: %s short=0x%04x", name(*existing), e.short_addr);
        heard(*existing);
        post_(ev::RoleAssigned{*existing, d});
        if (*existing == ev::Role::Button || d.endpoint == 0) {
            discover(*existing);
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
    if (records_[ev::indexOf(*role)].valid) {
        SUNSIM_LOGW(kTag, "Ignoring device 0x%04x: %s already assigned to a different device", e.short_addr,
                    name(*role));
        return;
    }
    SUNSIM_LOGI(kTag, "New device joined -> %s (from role manifest)", name(*role));
    assign(*role, e.ext_addr, e.short_addr);
}

void DeviceRegistry::onEndpointFound(const ev::EndpointFound &e)
{
    ev::DeviceRecord &d = records_[ev::indexOf(e.role)];
    if (e.endpoint == 0 || !d.valid) {
        return; // the adapter logged why; retried every kDiscoveryRetrySeconds
    }
    d.endpoint = e.endpoint;
    persist();
    post_(ev::RoleAssigned{e.role, d});
    if (e.role == ev::Role::Button) {
        SUNSIM_LOGI(kTag, "Button endpoint = %d, binding to local receiver", d.endpoint);
        post_(ev::Bind{e.role, d.ext_addr, d.short_addr, d.endpoint});
    } else {
        SUNSIM_LOGI(kTag, "%s (bulb) endpoint = %d", name(e.role), d.endpoint);
        post_(ev::RefreshLights{e.role});
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
    if (!records_[ev::indexOf(e.role)].valid) {
        return; // unpaired meanwhile
    }
    if (e.ok) {
        heard(e.role);
        return;
    }
    // A lost ping alone isn't enough (a busy network drops some); it must
    // also have been silent - no acks - for a while.
    const Liveness &l = liveness_[ev::indexOf(e.role)];
    if (l.last_heard_us == 0 || now_us_ - l.last_heard_us > kUnreachableSilenceUs) {
        SUNSIM_LOGD(kTag, "Ping to %s failed (err=0x%x status=0x%x)", name(e.role), e.error, e.status);
        setReachable(e.role, false);
    }
}

void DeviceRegistry::onAck(const ev::AckReceived &e)
{
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        if (records_[i].valid && records_[i].short_addr == e.short_addr) {
            heard(static_cast<ev::Role>(i));
            liveness_[i].rssi = e.rssi;
        }
    }
}

void DeviceRegistry::onButtonPressed(const ev::ButtonPressed &e)
{
    if (e.source == ev::Source::Zigbee && records_[ev::indexOf(ev::Role::Button)].valid) {
        heard(ev::Role::Button);
    }
}

void DeviceRegistry::onPairRequested(const ev::PairRequested &e)
{
    if (records_[ev::indexOf(e.role)].valid) {
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
    if (e.role) {
        clear(*e.role);
        SUNSIM_LOGI(kTag, "%s cleared", name(*e.role));
    } else {
        for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
            clear(static_cast<ev::Role>(i));
        }
        SUNSIM_LOGI(kTag, "All roles cleared");
    }
    persist();
}

void DeviceRegistry::onTick(const ev::Tick &e)
{
    now_us_ = e.uptime_us;
    ++seconds_;

    if (seconds_ % kDiscoveryRetrySeconds == 0) {
        for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
            if (records_[i].valid && records_[i].endpoint == 0) {
                SUNSIM_LOGI(kTag, "Retrying endpoint discovery for %s (short=0x%04x)", ev::roleName(i).data(),
                            records_[i].short_addr);
                discover(static_cast<ev::Role>(i));
            }
        }
    }

    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        // The button is a sleepy end device that won't answer; it counts as
        // heard when it's pressed instead.
        if (!records_[i].valid || static_cast<ev::Role>(i) == ev::Role::Button) {
            continue;
        }
        Liveness &l = liveness_[i];
        if (l.next_ping_s == 0) {
            l.next_ping_s = kFirstPingSeconds;
        }
        if (seconds_ >= l.next_ping_s) {
            l.next_ping_s = seconds_ + (l.reachable ? kPingSeconds : kUnreachablePingSeconds);
            post_(ev::Ping{static_cast<ev::Role>(i), records_[i].short_addr});
        }
    }

    ev::DevicesUpdated u{.devices = {}, .records = records_, .armed = armed_};
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        u.devices[i] = {.link          = link(static_cast<ev::Role>(i)),
                        .paired        = records_[i].valid,
                        .has_endpoint  = records_[i].endpoint != 0,
                        .last_heard_us = liveness_[i].last_heard_us,
                        .rssi          = liveness_[i].rssi,
                        .reachable     = liveness_[i].reachable};
    }
    post_(u);
}

} // namespace sunsim::core
