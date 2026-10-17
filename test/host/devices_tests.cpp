// Pairing, discovery, liveness and the role layout: DeviceRegistry and LightOutput.

#include "check.h"

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

void devicesTests()
{
    test("pairing assigns the armed role, persists, discovers, then lights it", [] {
        Harness h;
        h.tick(); // the program needs a time to compute targets
        h.post(PairRequested{kWallC});
        CHECK(h.all<OpenNetwork>().size() == 1);

        h.post(DeviceAnnounced{.ext_addr = 0xAA, .short_addr = 0x1234});
        const auto assigned = h.all<RoleAssigned>();
        CHECK(assigned.size() == 1 && assigned[0].role == kWallC && assigned[0].record.ext_addr == 0xAA);
        CHECK(!h.all<PersistRoles>().empty());
        const auto discover = h.all<Discover>();
        CHECK(discover.size() == 1 && discover[0].role == kWallC && discover[0].short_addr == 0x1234);

        h.clearLog();
        h.post(EndpointFound{.role = kWallC, .endpoint = 11});
        const auto lights = h.all<SetLight>();
        CHECK(lights.size() == 1 && lights[0].short_addr == 0x1234 && lights[0].endpoint == 11);
        CHECK(lights.size() == 1 && lights[0].power && *lights[0].power); // first command switches it on
    });

    test("a device that already holds a role is never claimed by `pair`", [] {
        Harness h;
        h.pairBulb(kWallA, 0xA1, 0x0001);
        h.post(PairRequested{kWallC});
        h.clearLog();
        h.post(DeviceAnnounced{.ext_addr = 0xA1, .short_addr = 0x0002}); // WallA power-cycles
        const auto assigned = h.all<RoleAssigned>();
        CHECK(assigned.size() == 1 && assigned[0].role == kWallA && assigned[0].record.short_addr == 0x0002);
        CHECK(h.all<RoleAssigned>().size() == 1); // WallC not assigned
    });

    test("unknown devices are ignored when nothing is armed", [] {
        Harness h;
        h.post(DeviceAnnounced{.ext_addr = 0xBEEF, .short_addr = 0x0005});
        CHECK(h.all<RoleAssigned>().empty());
        CHECK(h.all<PersistRoles>().empty());
    });

    test("liveness: acks -> online; 150 s of silence -> silent", [] {
        Harness h;
        h.tick();
        h.pairBulb(kWork, 0xB2, 0x0042);
        h.post(AckReceived{.short_addr = 0x0042, .rssi = -48});
        h.tick();
        auto snaps = h.all<StatusChanged>();
        CHECK(!snaps.empty() && snaps.back().snapshot->devices[kWork].link == DeviceLink::Online);
        CHECK(!snaps.empty() && snaps.back().snapshot->devices[kWork].rssi == -48);

        h.clearLog();
        h.tick(160);
        snaps = h.all<StatusChanged>();
        CHECK(!snaps.empty() && snaps.back().snapshot->devices[kWork].link == DeviceLink::Silent);
        CHECK(h.all<Ping>().size() >= 2); // pinged every 60 s meanwhile
    });

    test("endpoint discovery is retried every 15 s until found", [] {
        Harness h;
        h.post(PairRequested{kWallB});
        h.post(DeviceAnnounced{.ext_addr = 0xC3, .short_addr = 0x0003});
        h.post(EndpointFound{.role = kWallB, .endpoint = 0}); // no reply
        h.clearLog();
        h.tick(30);
        CHECK(h.all<Discover>().size() == 2);
    });

    test("unpairing a role mid-discovery stops its retries", [] {
        Harness h;
        h.post(PairRequested{kWallB});
        h.post(DeviceAnnounced{.ext_addr = 0xC3, .short_addr = 0x0003}); // no EndpointFound
        h.post(UnpairRequested{kWallB});
        h.clearLog();
        h.tick(45);
        CHECK(h.all<Discover>().empty());
        CHECK(h.all<Ping>().empty());
    });

    test("a ping that gets no answer at all counts as failed", [] {
        Harness h;
        h.tick();
        h.pairBulb(kWork, 0xB2, 0x0002);
        h.clearLog();
        h.tick(90); // pinged at 15 s and 75 s, never answered; silent since pairing
        const auto changes = h.all<ReachabilityChanged>();
        CHECK(changes.size() == 1 && changes[0].role == kWork && !changes[0].reachable);
    });

    test("button pairing binds it once its endpoint is found", [] {
        Harness h;
        h.post(PairRequested{kButton});
        h.post(DeviceAnnounced{.ext_addr = 0x18690AFFFE12620EULL, .short_addr = 0x2FED});
        h.clearLog();
        h.post(EndpointFound{.role = kButton, .endpoint = 1});
        const auto binds = h.all<Bind>();
        CHECK(binds.size() == 1 && binds[0].short_addr == 0x2FED && binds[0].endpoint == 1);
        CHECK(h.all<SetLight>().empty()); // never treated as a bulb
    });

    test("a missing bulb is paused, the others keep going, and it resumes when it answers", [] {
        Harness h;
        h.tick();
        h.pairBulb(kWallA, 0xA1, 0x0001);
        h.pairBulb(kWork, 0xB2, 0x0002); // never answers
        h.post(AckReceived{.short_addr = 0x0001, .rssi = -50});
        h.clearLog();

        h.tick(14);
        h.post(AckReceived{.short_addr = 0x0001, .rssi = -50});
        h.tick(1); // 15 s after boot: first pings
        const auto pings = h.all<Ping>();
        CHECK(pings.size() == 2);

        // WallA's ping is lost too, but it acked recently: still reachable.
        h.post(PingResult{.role = kWallA, .ok = false});
        // Work's ping fails and it was never heard after pairing... except
        // at pairing itself, so it needs 60 s of silence first.
        h.post(PingResult{.role = kWork, .ok = false});
        CHECK(h.all<ReachabilityChanged>().empty());

        h.tick(60);
        h.post(AckReceived{.short_addr = 0x0001, .rssi = -50});
        h.post(PingResult{.role = kWork, .ok = false});
        auto changes = h.all<ReachabilityChanged>();
        CHECK(changes.size() == 1 && changes[0].role == kWork && !changes[0].reachable);

        h.clearLog();
        const TimeReading later = at(14h); // the curve has moved on since the last update
        h.post(ProgramTick{.time = later, .transition = 60s});
        bool to_work = false, to_wall = false;
        for (const auto &l : h.all<SetLight>()) {
            to_work |= l.short_addr == 0x0002;
            to_wall |= l.short_addr == 0x0001;
        }
        CHECK(!to_work); // paused
        CHECK(to_wall);  // others unaffected (level/color moved since last time)

        h.clearLog();
        h.tick(15);
        CHECK(h.all<Ping>().size() >= 1); // unreachable bulbs are checked every 15 s

        h.clearLog();
        h.post(PingResult{.role = kWork, .ok = true}); // powered back on
        changes = h.all<ReachabilityChanged>();
        CHECK(changes.size() == 1 && changes[0].reachable);
        bool resumed_on = false;
        for (const auto &l : h.all<SetLight>()) {
            resumed_on |= l.short_addr == 0x0002 && l.power && *l.power;
        }
        CHECK(resumed_on); // re-applied in full at once
    });

    test("only changed values are re-sent", [] {
        Harness h;
        h.tick();
        h.pairBulb(kWallB, 0xC3, 0x0003);
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition = 60s}); // same time -> same target
        CHECK(h.all<SetLight>().empty());
    });

    test("unpair all clears every role and persists", [] {
        Harness h;
        h.pairBulb(kWallA, 0xA1, 0x0001);
        h.clearLog();
        h.post(UnpairRequested{});
        CHECK(h.all<RoleCleared>().size() == ev::kRoleCount);
        const auto persisted = h.all<PersistRoles>();
        CHECK(persisted.size() == 1 && !persisted[0].records[kWallA].valid);
    });

    test("the layout: five wall lights east to west, then work and button", [] {
        static_assert(kRoleCount == 7 && kLightCount == 6);
        static_assert(members(Group::Wall).size() == 5);
        static_assert(roleName(kWallA) == "WallA" && roleName(kWallE) == "WallE");
        static_assert(roleName(kWork) == "Work" && roleName(kButton) == "Button");
        static_assert(parseRole("wallc") == kWallC && !parseRole("WallF"));
        static_assert(slot(kWork) == 5 && kRoles[slot(kWallC)] == kWallC);
        CHECK(std::ranges::equal(lights(), std::span{kRoles}.first(6)));
        static_assert(kindOf(kWallA) == Kind::ColorBulb && kindOf(kButton) == Kind::Button);
        static_assert(isLight(kWork) && !isLight(kButton) && isButton(kButton) && !isButton(kWork));

        const auto wall = parseSelection("wall");
        CHECK(wall && selects(*wall, kWallE) && !selects(*wall, kWork));
        const auto work = parseSelection("WORK");
        CHECK(work && selects(*work, kWork) && !selects(*work, kWallA));
        const auto all = parseSelection("all");
        CHECK(all && std::ranges::all_of(kRoles, [&](Role r) { return selects(*all, r); }));
        CHECK(!parseSelection("lamp"));
    });

    test("unpairing a group clears just its roles", [] {
        Harness h;
        h.pairBulb(kWallA, 0xA1, 0x0001);
        h.pairBulb(kWallE, 0xA5, 0x0005);
        h.pairBulb(kWork, 0xB2, 0x0002);
        h.clearLog();
        h.post(UnpairRequested{Group::Wall});
        CHECK(h.all<RoleCleared>().size() == members(Group::Wall).size());
        const auto persisted = h.all<PersistRoles>();
        CHECK(persisted.size() == 1 && !persisted[0].records[kWallA].valid && !persisted[0].records[kWallE].valid &&
              persisted[0].records[kWork].valid);
    });

    test("addresses are checked against the layout by name; a known one joins as its role", [] {
        constexpr auto addresses =
            addressesOf(std::array{Address{"WallE", 0xE5}, Address{"work", 0xB2}, Address{"WallB", 0}});
        static_assert(addresses[kWallE] == 0xE5 && addresses[kWork] == 0xB2);
        static_assert(addresses[kWallA] == 0 && addresses[kWallB] == 0); // 0: none given
        // Wrong names, a role listed twice or an address given twice don't compile.

        EventBus          bus;
        core::CoreModules core{bus.post(), addresses};
        std::vector<RoleAssigned> assigned;
        core::wireCore(bus, core);
        bus.on<RoleAssigned>([&](const RoleAssigned &e) { assigned.push_back(e); });
        bus.post()(NetworkUp{});
        bus.post()(DeviceAnnounced{.ext_addr = 0xE5, .short_addr = 0x0005});
        bus.drain();
        CHECK(assigned.size() == 1 && assigned[0].role == kWallE);
    });

    test("each kind of light gets only the commands it takes", [] {
        const LightState amber{.on = true, .level = 80, .color = Xy{0.55f, 0.41f}};
        const LightState red{.on = true, .level = 200, .color = HueSat{0, 254}};
        const auto       to = [](const LightState &state) {
            return LightChange{.state = state, .level_transition = 60s, .color_transition = 60s};
        };

        // A color bulb: everything, then only what moves.
        const auto color = core::commandsFor(std::nullopt, to(amber), caps(Kind::ColorBulb));
        CHECK(color && color->power == true && color->level == 80 && colorOf<Xy>(*color));
        const auto dimmer = core::commandsFor(amber, to({.on = true, .level = 60, .color = amber.color}),
                                              caps(Kind::ColorBulb));
        CHECK(dimmer && !dimmer->power && dimmer->level == 60 && !dimmer->color);

        // A white bulb: xy as its nearest color temperature; a saturated color not at all.
        const LightState white = core::fitted(amber, caps(Kind::WhiteBulb));
        CHECK(colorOf<Mireds>(white) && colorOf<Mireds>(white)->value > 450 && colorOf<Mireds>(white)->value < 600);
        const auto alert = core::commandsFor(white, to(core::fitted(red, caps(Kind::WhiteBulb))), caps(Kind::WhiteBulb));
        CHECK(alert && alert->level == 200 && !alert->color);

        // A dimmable bulb: no color. A plug: only on and off.
        const auto dim = core::commandsFor(std::nullopt, to(amber), caps(Kind::DimmableBulb));
        CHECK(dim && dim->level == 80 && !dim->color);
        const LightState plugged = core::fitted(amber, caps(Kind::Plug));
        const auto       on      = core::commandsFor(std::nullopt, to(plugged), caps(Kind::Plug));
        CHECK(on && on->power == true && !on->level && !on->color);
        CHECK(!core::commandsFor(plugged, to(core::fitted(red, caps(Kind::Plug))), caps(Kind::Plug))); // still on
        const auto off = core::commandsFor(plugged, to(LightState{}), caps(Kind::Plug));
        CHECK(off && off->power == false);
    });

    test("role sets: lists, groups and none", [] {
        CHECK(parseRoleSet("WallB,walld") == roleSet({kWallB, Role{Group::Wall, 3}}));
        CHECK(parseRoleSet("wall,Work")->count() == kLightCount);
        CHECK(parseRoleSet("none")->none());
        CHECK(!parseRoleSet("WallB,attic"));
    });
}
