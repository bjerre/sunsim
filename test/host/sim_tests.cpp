// The whole app on simulated hardware (sunsim.sim): the core with a virtual
// clock and a Zigbee network of virtual bulbs and a button, run in virtual
// time from joining on.

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.sim;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

namespace {
// Each device's IEEE address: in installation.cppm's place, so each takes
// its role as it joins.
uint64_t extOf(Role role)
{
    return 0x00124B0000000000 + slot(role);
}

Addresses roomAddresses()
{
    Addresses addresses{};
    for (const Role role : kRoles) {
        addresses[role] = extOf(role);
    }
    return addresses;
}

constexpr uint16_t kRoomGroup = 0x5301;
std::vector<core::LightGroup> roomGroup()
{
    RoleSet all;
    for (const Role light : lights()) {
        all.set(slot(light));
    }
    return {{kRoomGroup, all}};
}

const Color kRed = HueSat{0, 254};

// The room's six lights and its button, powered on at noon; the bulbs in
// `groups`, if any, are in those groups too.
struct Room {
    sim::World                            world;
    std::shared_ptr<const StatusSnapshot> status;

    explicit Room(const std::vector<core::LightGroup> &groups = {}) : world(noon(), roomAddresses(), groups)
    {
        world.bus.on<StatusChanged>([this](const StatusChanged &e) { status = e.snapshot; });
        for (const Role role : kRoles) {
            world.network.add({.ext_addr = extOf(role), .kind = kindOf(role)});
            for (const core::LightGroup &g : groups) {
                if (contains(g.members, role)) {
                    world.network.joinGroup(extOf(role), g.id);
                }
            }
        }
        world.start();
    }

    [[nodiscard]] const sim::Bulb &bulb(Role role) const { return *world.network.bulb(extOf(role)); }
    [[nodiscard]] DeviceLink       link(Role role) const { return status ? status->devices[role].link : DeviceLink::Unpaired; }
};
} // namespace

const suite<"sim"> simTests = [] {
    "a simulated room joins, takes its roles and lights up with the sun"_test = [] {
        Room r;
        r.world.runFor(5s);
        for (const Role light : lights()) {
            expect(r.link(light) == DeviceLink::Online);
            expect(r.bulb(light).on && r.bulb(light).to > 0 && r.bulb(light).color.has_value());
        }
        expect(r.link(kButton) != DeviceLink::Unpaired);
        expect(that % r.world.network.counts().lost == 0u);
    };

    "virtual time: an hour runs at once, and the clock keeps up"_test = [] {
        Room r;
        r.world.runFor(1h);
        expect(r.status && r.status->time.local == noon().local + 1h);
    };

    "a double press turns the room red, one group command per step"_test = [] {
        Room r{roomGroup()};
        r.world.runFor(5s);
        const auto before = r.world.network.counts();
        r.world.network.press(extOf(kButton), Press::Double);
        r.world.runFor(300ms);
        const auto after = r.world.network.counts();
        expect(that % (after.group_commands - before.group_commands) == 2u); // the red, then the level
        expect(that % (after.unicasts - before.unicasts) == 0u);
        for (const Role light : lights()) {
            expect(r.bulb(light).color == kRed);
        }

        r.world.runFor(60s); // a beat a second
        expect(that % r.world.network.counts().refused_broadcasts == 0u); // the queue keeps inside the stack's limit
    };

    "a bulb out of reach goes silent, then catches up when it's back"_test = [] {
        Room r;
        r.world.runFor(5s);
        r.world.network.setReachable(extOf(kWallC), false);
        r.world.network.press(extOf(kButton), Press::Double);
        r.world.runFor(200s);
        expect(r.link(kWallC) == DeviceLink::Silent);
        expect(r.bulb(kWallC).color != kRed && r.bulb(kWallA).color == kRed);

        r.world.network.setReachable(extOf(kWallC), true);
        r.world.runFor(30s); // its next ping answers
        expect(r.link(kWallC) == DeviceLink::Online);
        expect(r.bulb(kWallC).color == kRed);
    };

    "a button that isn't bound isn't heard"_test = [] {
        sim::World                            world{noon(), roomAddresses()};
        std::shared_ptr<const StatusSnapshot> status;
        world.bus.on<StatusChanged>([&](const StatusChanged &e) { status = e.snapshot; });
        world.network.add({.ext_addr = extOf(kButton), .kind = Kind::Button});
        world.network.press(extOf(kButton), Press::Double); // before it has joined, let alone been bound
        world.start();
        world.runFor(5s);
        expect(status && status->zones[ZoneId{}].mode == Mode::Daylight);
    };
};
