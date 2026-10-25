export module sunsim.core:device_registry;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::core {

// Which device holds which role, and whether it's alive.
//  - Pairing: a device announcing itself while a role is armed (console
//    `pair`) gets that role, unless it already holds one; otherwise its
//    address in the installation (ev::kAddresses) decides; anything else is
//    ignored.
//  - Discovery (a flow per role): finds the device's endpoint
//    (Discover/EndpointFound), retried every 15 s until it's known; the
//    button is then bound (Bind).
//  - Liveness (a flow per light): acks, ping replies, announces and button
//    presses count as "heard". Lights are pinged 15 s after the network is
//    up (or after pairing), plus their turn (spread across the minute by
//    slot), then every 60 s. A failed or unanswered ping
//    with nothing heard for a minute marks a light unreachable
//    (ReachabilityChanged): its commands pause -
//    sending to a missing device ties up the stack's buffers with retries
//    and starves everything else - and it's pinged every 15 s until it
//    answers anything, when it's marked reachable again and its light
//    re-applied. Heard within 150 s (and reachable) shows as Online in
//    DevicesUpdated, otherwise Silent.
// Persists through PersistRoles; never touches storage or the radio itself.
class DeviceRegistry {
public:
    // addresses: who takes which role when it joins (ev::kAddressOf).
    DeviceRegistry(ev::Post post, ev::Addresses addresses);

    void on(const ev::RolesLoaded &e);
    void on(const ev::NetworkUp &e);
    void on(const ev::DeviceAnnounced &e);
    void on(const ev::EndpointFound &e);
    void on(const ev::BindResult &e);
    void on(const ev::PingResult &e);
    void on(const ev::AckReceived &e);
    void on(const ev::ButtonPressed &e);
    void on(const ev::PairRequested &e);
    void on(const ev::PairCanceled &e);
    void on(const ev::UnpairRequested &e);
    void on(const ev::Tick &e);
    void on(const ev::Wake &e) { timer_.tick(e.at); }

private:
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
    [[nodiscard]] std::optional<ev::Role> roleFromAddress(uint64_t ext_addr) const noexcept;
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
    ev::Flow keepAlive(ev::Role light);

    ev::Post                     post_;
    ev::Addresses                addresses_;
    ev::DeviceRecords            records_{};
    ev::PerRole<Liveness>        liveness_{};
    std::optional<ev::Role>      armed_;

    ev::Timer                    timer_{[this](ev::Instant at) { post_(ev::WakeAt{at}); }};
    bool                         network_up_ = false; // nothing is sent before
    ev::Inbox<ev::NetworkUp>     network_{timer_};
    ev::Inbox<ev::EndpointFound> endpoints_{timer_};
    ev::Inbox<ev::PingResult>    pings_{timer_};
    ev::PerRole<ev::Flow>        discovery_;  // last, so they're destroyed
    ev::PerRole<ev::Flow>        keep_alive_; // before what they wait on (lights only)
};

} // namespace sunsim::core

namespace sunsim::core {

using namespace std::chrono_literals;

namespace {
constexpr const char *kTag = "REGISTRY";

constexpr auto kDiscoveryRetry   = 15s;
constexpr auto kFirstPing        = 15s;  // after the network is up, or pairing
constexpr auto kPingInterval     = 60s;  // reachable lights
constexpr auto kUnreachablePing  = 15s;  // until they answer again
constexpr auto kUnreachableAfter = 60s;  // of silence, plus a failed ping
constexpr auto kOnlineWindow     = 150s; // shown as online: a couple of missed pings

// The lights take turns: each is first pinged at its own point in the
// interval, by slot, so the pings spread out instead of all going at once.
constexpr ev::BootClock::duration pingTurn(ev::Role light) noexcept
{
    return kPingInterval * ev::slot(light) / ev::kRoleCount;
}

std::string_view name(ev::Role role) noexcept
{
    return ev::roleName(role);
}

// A filter for events about `role`.
constexpr auto about(ev::Role role) noexcept
{
    return [role](const auto &e) { return e.role == role; };
}
} // namespace

DeviceRegistry::DeviceRegistry(ev::Post post, ev::Addresses addresses)
    : post_(std::move(post)), addresses_(addresses)
{
}

std::optional<ev::Role> DeviceRegistry::roleByExt(uint64_t ext_addr) const noexcept
{
    for (const ev::Role role : ev::kRoles) {
        if (record(role).valid && record(role).ext_addr == ext_addr) {
            return role;
        }
    }
    return std::nullopt;
}

std::optional<ev::Role> DeviceRegistry::roleFromAddress(uint64_t ext_addr) const noexcept
{
    for (const ev::Role role : ev::kRoles) {
        if (ext_addr != 0 && addresses_[role] == ext_addr) {
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
        ev::log::info(kTag, "{} is answering again - resuming its light", name(role));
        if (ev::isLight(role)) {
            post_(ev::RefreshLights{role});
        }
    } else {
        ev::log::warn(kTag, "{} not answering - pausing its light commands (powered off or out of range?)",
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
    if (ev::isLight(role)) {
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

void DeviceRegistry::on(const ev::RolesLoaded &e)
{
    records_ = *e.records;
    for (const ev::Role role : ev::kRoles) {
        if (!record(role).valid) {
            continue;
        }
        post_(ev::RoleAssigned{role, record(role)});
        if (record(role).endpoint == 0) {
            discovery_[role] = findEndpoint(role);
        }
        if (ev::isLight(role)) {
            keep_alive_[role] = keepAlive(role);
        }
    }
}

void DeviceRegistry::on(const ev::NetworkUp &e)
{
    network_up_ = true;
    network_.deliver(e, e.at);
}

void DeviceRegistry::on(const ev::DeviceAnnounced &e)
{
    if (const auto existing = roleByExt(e.ext_addr)) {
        ev::DeviceRecord &d = record(*existing);
        if (d.short_addr != e.short_addr) {
            d.short_addr = e.short_addr;
            persist();
        }
        ev::log::info(kTag, "Known device rejoined: {} short=0x{:04x}", name(*existing), e.short_addr);
        heard(*existing);
        post_(ev::RoleAssigned{*existing, d});
        if (ev::isButton(*existing) || d.endpoint == 0) {
            discovery_[*existing] = findEndpoint(*existing); // a button is re-bound
        } else {
            post_(ev::RefreshLights{*existing});
        }
        return;
    }

    if (armed_) {
        const ev::Role role = *armed_;
        armed_.reset();
        ev::log::info(kTag, "Paired ext=0x{:016x} short=0x{:04x} -> {}", e.ext_addr,
                      e.short_addr, name(role));
        assign(role, e.ext_addr, e.short_addr);
        return;
    }

    const auto role = roleFromAddress(e.ext_addr);
    if (!role) {
        ev::log::warn(kTag,
                      "Ignoring unknown device 0x{:04x} (ext=0x{:016x}): not in the installation's addresses (use "
                      "`pair <role>` to claim it)",
                      e.short_addr, e.ext_addr);
        return;
    }
    if (record(*role).valid) {
        ev::log::warn(kTag, "Ignoring device 0x{:04x}: {} already assigned to a different device", e.short_addr,
                      name(*role));
        return;
    }
    ev::log::info(kTag, "New device joined -> {} (by its address)", name(*role));
    assign(*role, e.ext_addr, e.short_addr);
}

void DeviceRegistry::on(const ev::EndpointFound &e)
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
        ev::log::info(kTag, "Retrying endpoint discovery for {} (short=0x{:04x})", name(role), record(role).short_addr);
    }
}

void DeviceRegistry::endpointFound(ev::Role role, uint8_t endpoint)
{
    ev::DeviceRecord &d = record(role);
    d.endpoint          = endpoint;
    persist();
    post_(ev::RoleAssigned{role, d});
    if (ev::isButton(role)) {
        ev::log::info(kTag, "{} endpoint = {}, binding to local receiver", name(role), d.endpoint);
        post_(ev::Bind{role, d.ext_addr, d.short_addr, d.endpoint});
    } else {
        ev::log::info(kTag, "{} (light) endpoint = {}", name(role), d.endpoint);
        post_(ev::RefreshLights{role});
    }
}

void DeviceRegistry::on(const ev::BindResult &e)
{
    if (e.ok) {
        ev::log::info(kTag, "Bound {} to local receiver endpoint", name(e.role));
    } else {
        ev::log::error(kTag, "Failed to bind {}", name(e.role));
    }
}

void DeviceRegistry::on(const ev::PingResult &e)
{
    pings_.deliver(e);
}

// A button is a sleepy end device that won't answer pings; it counts as
// heard when it's pressed instead.
ev::Flow DeviceRegistry::keepAlive(ev::Role light)
{
    if (!network_up_) {
        co_await network_.next();
    }
    co_await timer_.after(kFirstPing + pingTurn(light));
    for (;;) {
        const ev::Instant sent = timer_.now();
        post_(ev::Ping{light, record(light).short_addr});
        const auto reply = co_await pings_.next(about(light), sent + kUnreachablePing);
        if (reply && reply->ok) {
            heard(light);
        } else {
            // A lost ping alone isn't enough (a busy network drops some); it
            // must also have been silent - no acks - for a while.
            const Liveness &l = liveness(light);
            if (!l.last_heard || timer_.now() - *l.last_heard > kUnreachableAfter) {
                ev::log::debug(kTag, "Ping to {} failed (err=0x{:x} status=0x{:x})", name(light), reply ? reply->error : -1,
                               reply ? reply->status : -1);
                setReachable(light, false);
            }
        }
        co_await timer_.until(sent + (liveness(light).reachable ? kPingInterval : kUnreachablePing));
    }
}

void DeviceRegistry::on(const ev::AckReceived &e)
{
    for (const ev::Role role : ev::kRoles) {
        if (record(role).valid && record(role).short_addr == e.short_addr) {
            heard(role);
            liveness(role).rssi = e.rssi;
        }
    }
}

void DeviceRegistry::on(const ev::ButtonPressed &e)
{
    if (e.source != ev::Source::Zigbee) {
        return;
    }
    if (record(e.button).valid) {
        heard(e.button);
    }
}

void DeviceRegistry::on(const ev::PairRequested &e)
{
    if (record(e.role).valid) {
        ev::log::warn(kTag, "{} is already assigned; the next device to join will replace it", name(e.role));
    }
    armed_ = e.role;
    post_(ev::OpenNetwork{});
    ev::log::info(kTag,
                  "Armed {}: factory-reset / power on the device now. The next device that joins and doesn't already "
                  "hold a role becomes {}.",
                  name(e.role), name(e.role));
}

void DeviceRegistry::on(const ev::PairCanceled &)
{
    armed_.reset();
    ev::log::info(kTag, "Pairing canceled");
}

void DeviceRegistry::on(const ev::UnpairRequested &e)
{
    for (const ev::Role role : ev::kRoles) {
        if (ev::selects(e.roles, role)) {
            clear(role);
            if (!std::holds_alternative<ev::AllRoles>(e.roles)) {
                ev::log::info(kTag, "{} cleared", name(role));
            }
        }
    }
    if (std::holds_alternative<ev::AllRoles>(e.roles)) {
        ev::log::info(kTag, "All roles cleared");
    }
    persist();
}

void DeviceRegistry::on(const ev::Tick &e)
{
    timer_.tick(e.at);
    publishDevices();
}

void DeviceRegistry::publishDevices()
{
    ev::PerRole<ev::DeviceStatus> devices{};
    for (const ev::Role role : ev::kRoles) {
        const Liveness &l = liveness(role);
        devices[role]     = {.link         = link(role),
                             .paired       = record(role).valid,
                             .has_endpoint = record(role).endpoint != 0,
                             .last_heard   = l.last_heard,
                             .rssi         = l.rssi,
                             .reachable    = l.reachable};
    }
    post_(ev::DevicesUpdated{.devices = devices, .records = records_, .armed = armed_});
}

} // namespace sunsim::core
