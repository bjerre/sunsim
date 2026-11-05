export module sunsim.sim:sim_network;

import std.compat;
import sunsim.ev;
import :scheduler;

export namespace sunsim::sim {

// A device in the simulated network, as it's set up: what it is, and how
// long a frame takes to reach it.
struct Device {
    uint64_t                ext_addr = 0;
    ev::Kind                kind     = ev::Kind::ColorBulb;
    uint8_t                 endpoint = 11;
    ev::BootClock::duration latency  = std::chrono::milliseconds{20}; // each way
    int8_t                  rssi     = -55;
};

// A light as its ZCL clusters hold it. The level moves to its target over
// the command's transition, as a bulb's does; the color is the last one
// sent (when it's shown is all the tests need).
struct Bulb {
    bool                     on    = false;
    std::optional<ev::Color> color;
    uint8_t                  from  = 0; // the level moving from `from` to `to`
    uint8_t                  to    = 0;
    ev::Instant              since{};
    ev::BootClock::duration  over{};

    // What it shows at `at`: 0 when it's off.
    [[nodiscard]] uint8_t levelAt(ev::Instant at) const noexcept;
};

// The board's radio, simulated: platform::ZigbeeAdapter's events, answered
// as the network would. Devices join while the network is open (pairing,
// or the few minutes after it's up), each bulb holds what it's sent, and a
// device out of reach answers nothing. Like the real stack, it holds a
// broadcast (a group command) for kBroadcastHeld and has room for
// kBroadcastSlots at a time: past that, a group command is refused. Group
// membership is set by hand (joinGroup()) until sunsim adds bulbs to groups
// itself.
class SimNetwork {
public:
    static constexpr std::size_t          kBroadcastSlots = 9;
    static constexpr std::chrono::seconds kBroadcastHeld{9};
    static constexpr std::chrono::milliseconds kSendGap{25}; // as the ZigbeeAdapter paces commands
    static constexpr std::chrono::seconds kNoAnswer{3};      // a request to a device out of reach: given up on
    static constexpr std::chrono::seconds kForming{2};       // from start() to the network being up

    // What the network has carried, for tests.
    struct Counts {
        uint32_t unicasts           = 0;
        uint32_t group_commands     = 0;
        uint32_t refused_broadcasts = 0; // no room in the broadcast table
        uint32_t lost               = 0; // sent to a device out of reach
    };

    SimNetwork(ev::Post post, Scheduler &scheduler);

    // The network forms: NetworkUp kForming later, and it's open for a while.
    void start();
    // A device is powered on: it joins as soon as the network is open.
    // Returns its short address.
    uint16_t add(const Device &device);
    void     setReachable(uint64_t ext_addr, bool reachable);
    void     joinGroup(uint64_t ext_addr, uint16_t group);
    // A press on a button, which arrives if the button is bound to us.
    void press(uint64_t button, ev::Press press);

    [[nodiscard]] const Bulb   *bulb(uint64_t ext_addr) const;
    [[nodiscard]] const Counts &counts() const noexcept { return counts_; }

    void on(const ev::NetworkUp &);
    void on(const ev::SendsWaiting &);
    void on(const ev::SendSlot &) { slot_due_ = false; }
    void on(const ev::SendLight &e);
    void on(const ev::Discover &e);
    void on(const ev::Bind &e);
    void on(const ev::Ping &e);
    void on(const ev::OpenNetwork &e);
    void on(const ev::FactoryReset &);

private:
    struct Node {
        Device                device;
        uint16_t              short_addr = 0;
        bool                  joined     = false;
        bool                  reachable  = true;
        bool                  bound      = false; // a button's On/Off output, to us
        Bulb                  bulb;
        std::vector<uint16_t> groups;
    };

    [[nodiscard]] Node *byShort(uint16_t short_addr);
    [[nodiscard]] Node *byExt(uint64_t ext_addr);
    void                joinIfOpen(Node &node);
    void                apply(Node &node, const ev::SendLight &e);
    // A request's answer: after the round trip, or kNoAnswer's `missing` if
    // the device is out of reach (nothing, if that's nullopt).
    void answer(const Node *node, ev::Event reply, std::optional<ev::Event> missing);

    ev::Post                 post_;
    Scheduler               &scheduler_;
    std::vector<Node>        nodes_;
    ev::Instant              open_until_{};
    std::deque<ev::Instant>  broadcasts_; // held, oldest first
    bool                     slot_due_ = false;
    Counts                   counts_{};
};

static_assert(ev::Radio<SimNetwork>);

} // namespace sunsim::sim

namespace sunsim::sim {

namespace {
// The button, as the ZigbeeAdapter names its presses.
constexpr ev::Role kButton = [] {
    const auto button = std::ranges::find_if(ev::kRoles, ev::isButton);
    return button == ev::kRoles.end() ? ev::kRoles.front() : *button;
}();

constexpr uint16_t kFirstShort = 0x1001;
} // namespace

uint8_t Bulb::levelAt(ev::Instant at) const noexcept
{
    if (!on) {
        return 0;
    }
    if (over.count() <= 0 || at >= since + over) {
        return to;
    }
    const double done = at <= since ? 0.0 : static_cast<double>((at - since).count()) / static_cast<double>(over.count());
    return static_cast<uint8_t>(std::lround(from + (to - from) * done));
}

SimNetwork::SimNetwork(ev::Post post, Scheduler &scheduler) : post_(std::move(post)), scheduler_(scheduler) {}

void SimNetwork::start()
{
    const ev::Instant up = scheduler_.now() + kForming;
    scheduler_.at(up, ev::NetworkUp{.rejoined = false, .at = up});
}

// Up: open, as the ZigbeeAdapter opens it then, for the devices waiting to join.
void SimNetwork::on(const ev::NetworkUp &)
{
    on(ev::OpenNetwork{});
}

uint16_t SimNetwork::add(const Device &device)
{
    Node node;
    node.device     = device;
    node.short_addr = static_cast<uint16_t>(kFirstShort + nodes_.size());
    nodes_.push_back(std::move(node));
    joinIfOpen(nodes_.back());
    return nodes_.back().short_addr;
}

void SimNetwork::setReachable(uint64_t ext_addr, bool reachable)
{
    if (Node *node = byExt(ext_addr)) {
        node->reachable = reachable;
    }
}

void SimNetwork::joinGroup(uint64_t ext_addr, uint16_t group)
{
    if (Node *node = byExt(ext_addr); node && !std::ranges::contains(node->groups, group)) {
        node->groups.push_back(group);
    }
}

void SimNetwork::press(uint64_t button, ev::Press press)
{
    const Node *node = byExt(button);
    if (node && node->joined && node->reachable && node->bound) {
        const ev::Instant at = scheduler_.now() + node->device.latency;
        scheduler_.at(at, ev::ButtonPressed{.button = kButton, .press = press, .source = ev::Source::Zigbee, .at = at});
    }
}

const Bulb *SimNetwork::bulb(uint64_t ext_addr) const
{
    const auto node = std::ranges::find(nodes_, ext_addr, [](const Node &n) { return n.device.ext_addr; });
    return node == nodes_.end() ? nullptr : &node->bulb;
}

void SimNetwork::on(const ev::SendsWaiting &)
{
    if (!slot_due_) {
        slot_due_ = true;
        scheduler_.after(kSendGap, ev::SendSlot{.at = scheduler_.now() + kSendGap});
    }
}

void SimNetwork::on(const ev::SendLight &e)
{
    if (!e.to.group) {
        ++counts_.unicasts;
        Node *node = byShort(e.to.address);
        if (!node || !node->reachable) {
            ++counts_.lost;
            return;
        }
        apply(*node, e);
        scheduler_.after(2 * node->device.latency,
                         ev::AckReceived{.short_addr = node->short_addr, .rssi = node->device.rssi, .status = 0});
        return;
    }
    const ev::Instant now = scheduler_.now();
    while (!broadcasts_.empty() && now - broadcasts_.front() >= kBroadcastHeld) {
        broadcasts_.pop_front();
    }
    if (broadcasts_.size() >= kBroadcastSlots) {
        ++counts_.refused_broadcasts;
        return;
    }
    broadcasts_.push_back(now);
    ++counts_.group_commands;
    for (Node &node : nodes_) {
        if (node.joined && node.reachable && std::ranges::contains(node.groups, e.to.address)) {
            apply(node, e);
        }
    }
}

void SimNetwork::on(const ev::Discover &e)
{
    const Node   *node     = byShort(e.short_addr);
    const uint8_t endpoint = node && node->device.kind == ev::kindOf(e.role) ? node->device.endpoint : 0;
    answer(node, ev::EndpointFound{.role = e.role, .endpoint = endpoint}, ev::EndpointFound{.role = e.role, .endpoint = 0});
}

void SimNetwork::on(const ev::Bind &e)
{
    Node *node = byShort(e.short_addr);
    if (node && node->reachable) {
        node->bound = true;
    }
    answer(node, ev::BindResult{.role = e.role, .ok = true}, ev::BindResult{.role = e.role, .ok = false});
}

void SimNetwork::on(const ev::Ping &e)
{
    answer(byShort(e.short_addr), ev::PingResult{.role = e.role, .ok = true},
           ev::PingResult{.role = e.role, .ok = false, .error = 1, .status = -1});
}

void SimNetwork::on(const ev::OpenNetwork &e)
{
    open_until_ = scheduler_.now() + e.duration;
    for (Node &node : nodes_) {
        joinIfOpen(node);
    }
}

void SimNetwork::on(const ev::FactoryReset &)
{
    // The board would restart with an empty network: every device leaves,
    // and joins again when it's open.
    for (Node &node : nodes_) {
        node.joined = false;
        node.bound  = false;
        node.groups.clear();
    }
    broadcasts_.clear();
    start();
}

SimNetwork::Node *SimNetwork::byShort(uint16_t short_addr)
{
    const auto node = std::ranges::find(nodes_, short_addr, &Node::short_addr);
    return node == nodes_.end() || !node->joined ? nullptr : &*node;
}

SimNetwork::Node *SimNetwork::byExt(uint64_t ext_addr)
{
    const auto node = std::ranges::find(nodes_, ext_addr, [](const Node &n) { return n.device.ext_addr; });
    return node == nodes_.end() ? nullptr : &*node;
}

void SimNetwork::joinIfOpen(Node &node)
{
    if (node.joined || !node.reachable || scheduler_.now() >= open_until_) {
        return;
    }
    node.joined = true;
    scheduler_.after(node.device.latency,
                     ev::DeviceAnnounced{.ext_addr = node.device.ext_addr, .short_addr = node.short_addr});
}

void SimNetwork::apply(Node &node, const ev::SendLight &e)
{
    Bulb             &b  = node.bulb;
    const ev::Instant at = scheduler_.now() + node.device.latency;
    if (e.power) {
        b.on = *e.power;
    } else if (e.level) {
        b.from  = b.levelAt(at);
        b.to    = *e.level;
        b.since = at;
        b.over  = std::chrono::duration_cast<ev::BootClock::duration>(e.transition);
        b.on    = *e.level > 0; // Move to Level with On/Off
    } else if (e.color) {
        b.color = e.color;
    }
}

void SimNetwork::answer(const Node *node, ev::Event reply, std::optional<ev::Event> missing)
{
    if (node && node->reachable) {
        scheduler_.after(2 * node->device.latency, std::move(reply));
    } else if (missing) {
        scheduler_.after(kNoAnswer, std::move(*missing));
    }
}

} // namespace sunsim::sim
