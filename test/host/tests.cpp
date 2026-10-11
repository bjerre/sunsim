// Host tests: the real core modules, wired with the same core::wireCore()
// as the firmware, driven by events; a recorder captures everything posted.

import std.compat;
import sunsim.ev;
import sunsim.core;

using namespace sunsim;
using namespace sunsim::ev;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

constexpr Role kWallA{Group::Wall, 0}; // the east end
constexpr Role kWallB{Group::Wall, 1};
constexpr Role kWallC{Group::Wall, 2};
constexpr Role kWallE{Group::Wall, 4}; // the west end
constexpr Role kWork{Group::Work};
constexpr Role kButton{Group::Button};

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

// A weekday at 12:00, clock set.
TimeReading noon()
{
    TimeReading t;
    t.local.tm_wday = 2; // Tuesday
    t.local.tm_yday = 171;
    t.local.tm_hour = 12;
    t.valid         = true;
    t.utc_offset_hours = -5;
    return t;
}

// The core, wired like the firmware, plus a log of every event posted.
struct Harness {
    EventBus                 bus;
    core::CoreModules        core{bus.post(), {}};
    std::vector<Event>       log;
    Instant                  now{};

    Harness()
    {
        core::wireCore(bus, core);
        // Record everything posted, including platform-bound events.
        recordAll(std::make_index_sequence<std::variant_size_v<Payload>>{});
        post(NetworkUp{}); // at time 0, so "15 s after the network is up" is 15 s in
        clearLog();
    }

    template <std::size_t... I> void recordAll(std::index_sequence<I...>)
    {
        (bus.on<std::variant_alternative_t<I, Payload>>(
             [this](const std::variant_alternative_t<I, Payload> &e) { log.emplace_back(e); }),
         ...);
    }

    void post(Event e)
    {
        bus.post()(std::move(e));
        bus.drain();
    }

    void tick(int seconds = 1, TimeReading time = noon())
    {
        for (int i = 0; i < seconds; ++i) {
            now += 1s;
            post(Tick{.at = now, .time = time});
        }
    }

    template <class T> std::vector<T> all() const
    {
        std::vector<T> out;
        for (const auto &e : log) {
            if (const auto *p = std::get_if<T>(&e.data)) {
                out.push_back(*p);
            }
        }
        return out;
    }

    void clearLog() { log.clear(); }

    // Pair `role` through the console flow and complete discovery.
    void pairBulb(Role role, uint64_t ext, uint16_t short_addr, uint8_t ep = 1)
    {
        post(PairRequested{role});
        post(DeviceAnnounced{.ext_addr = ext, .short_addr = short_addr});
        post(EndpointFound{.role = role, .endpoint = ep});
    }
};

void test(const char *name, const std::function<void()> &body)
{
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

} // namespace

int main()
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

    test("double press -> red alert pulses; single press -> daylight resumes", [] {
        Harness h;
        h.tick();
        h.pairBulb(kWallA, 0xA1, 0x0001);
        h.pairBulb(kWork, 0xB2, 0x0002);
        h.clearLog();

        h.post(ButtonPressed{.press = Press::Double, .source = Source::Zigbee, .at = h.now});
        auto modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::RedAlert);
        auto red = h.all<SetLight>();
        CHECK(red.size() == 2); // one immediate pulse per bulb
        CHECK(red.size() == 2 && red[0].hue_sat && red[0].hue_sat->first == 0 && red[0].hue_sat->second == 254);

        h.clearLog();
        h.tick(3);
        const auto pulses = h.all<SetLight>();
        CHECK(pulses.size() == 6);
        CHECK(pulses.size() == 6 && pulses[0].level && pulses[2].level && *pulses[0].level != *pulses[2].level); // bright/dim alternate
        CHECK(pulses.size() == 6 && !pulses[0].hue_sat); // already red: only the level moves

        h.clearLog();
        h.post(ButtonPressed{.press = Press::Single, .source = Source::Zigbee, .at = h.now});
        modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight);
        const auto back = h.all<SetLight>();
        CHECK(back.size() == 2 && back[0].level && !back[0].hue_sat && (back[0].mireds || back[0].xy)); // daylight at once

    });

    test("hold -> all off; the daylight program is paused until resumed", [] {
        Harness h;
        h.tick();
        h.pairBulb(kWallB, 0xC3, 0x0003);
        h.clearLog();
        h.post(ButtonPressed{.press = Press::Hold, .source = Source::Console, .at = h.now});
        const auto off = h.all<SetLight>();
        CHECK(off.size() == 1 && off[0].power && !*off[0].power);

        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        CHECK(h.all<LightTarget>().empty());
        CHECK(h.all<SetLight>().empty());
    });

    test("a bulb paired during red alert joins in red", [] {
        Harness h;
        h.tick();
        h.post(ButtonPressed{.press = Press::Double, .source = Source::Console, .at = h.now});
        h.clearLog();
        h.pairBulb(kWallC, 0xC3, 0x0003);
        const auto lights = h.all<SetLight>();
        CHECK(lights.size() == 1 && lights[0].power && *lights[0].power);
        CHECK(lights.size() == 1 && lights[0].hue_sat && lights[0].hue_sat->first == 0);
    });

    test("ambiguous On/Toggle: two within 450 ms = double, one alone = single", [] {
        Harness h;
        h.tick();
        h.post(ButtonPressed{.press = Press::Ambiguous, .at = h.now});
        h.post(ButtonPressed{.press = Press::Ambiguous, .at = h.now + 200ms});
        auto modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::RedAlert);

        Harness g;
        g.tick();
        g.post(ButtonPressed{.press = Press::Ambiguous, .at = g.now});
        CHECK(g.all<ModeChanged>().empty()); // still waiting for a possible second click
        g.tick();
        modes = g.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight);
    });

    test("ambiguous then a definite press within the window: the definite press wins", [] {
        Harness h;
        h.tick();
        h.post(ButtonPressed{.press = Press::Ambiguous, .at = h.now});
        h.post(ButtonPressed{.press = Press::Hold, .at = h.now + 100ms});
        auto modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Off);
        h.tick(); // nothing left pending
        CHECK(h.all<ModeChanged>().size() == 1);
    });

    test("ambiguous: a second click after the window is a new first click", [] {
        Harness h;
        h.tick();
        h.post(ButtonPressed{.press = Press::Ambiguous, .at = h.now});
        h.post(ButtonPressed{.press = Press::Ambiguous, .at = h.now + 600ms}); // too late for a double
        auto modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight); // the first was a single
        h.post(ButtonPressed{.press = Press::Ambiguous, .at = h.now + 800ms}); // pairs with the second
        modes = h.all<ModeChanged>();
        CHECK(modes.size() == 2 && modes[1].mode == Mode::RedAlert);
    });

    test("clock not set -> lights held off", [] {
        Harness h;
        TimeReading unset = noon();
        unset.valid       = false;
        h.tick(1, unset);
        h.pairBulb(kWallA, 0xA1, 0x0001);
        h.clearLog();
        h.post(ProgramTick{.time = unset, .transition = 60s});
        const auto targets = h.all<LightTarget>();
        CHECK(targets.size() == ev::kBulbCount);
        for (const auto &t : targets) {
            CHECK(!t.state.on);
        }
    });

    test("intensity scales the target level; ProgramTick ramps over the interval", [] {
        Harness h;
        h.tick();
        Peaks peaks   = kDefaultPeaks;
        peaks[kWallA] = 50;
        h.post(ConfigChanged{.config = {}, .peaks = peaks});
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        const auto targets = h.all<LightTarget>();
        CHECK(targets.size() == ev::kBulbCount);
        if (targets.size() == ev::kBulbCount) {
            CHECK(targets[0].state.on && targets[1].state.on);
            CHECK(targets[0].state.level * 2 <= targets[1].state.level + 2); // WallA ~half of WallB
            CHECK(targets[0].level_transition == 60s && targets[0].color_transition == 60s);
        }
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
        TimeReading later = noon();
        later.local.tm_hour = 14; // the curve has moved on since the last update
        h.post(ProgramTick{.time = later, .transition = 60s});
        bool to_work = false, to_wall = false;
        for (const auto &l : h.all<SetLight>()) {
            to_work |= l.short_addr == 0x0002;
            to_wall |= l.short_addr == 0x0001;
        }
        CHECK(!to_work); // paused
        CHECK(to_wall);  // others unaffected (level/colour moved since last time)

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

    test("the bus delivers every event posted from many threads, in each poster's order", [] {
        EventBus         bus;
        std::vector<int> seen[2];
        bus.on<AckReceived>([&](const AckReceived &a) { seen[a.status].push_back(a.short_addr); });
        constexpr int kPerThread = 20000;
        std::vector<std::jthread> posters;
        for (uint8_t t = 0; t < 2; ++t) {
            posters.emplace_back([&bus, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    bus.post()(AckReceived{.short_addr = static_cast<uint16_t>(i), .status = t});
                }
            });
        }
        std::size_t delivered = 0;
        while (delivered < 2 * kPerThread) {
            bus.wait();
            bus.process();
            delivered = seen[0].size() + seen[1].size();
        }
        posters.clear(); // join
        for (const auto &s : seen) {
            CHECK(s.size() == kPerThread && std::ranges::is_sorted(s));
        }
    });

    test("the layout: five wall lights east to west, then work and button", [] {
        static_assert(kRoleCount == 7 && kBulbCount == 6);
        static_assert(members(Group::Wall).size() == 5);
        static_assert(roleName(kWallA) == "WallA" && roleName(kWallE) == "WallE");
        static_assert(roleName(kWork) == "Work" && roleName(kButton) == "Button");
        static_assert(parseRole("wallc") == kWallC && !parseRole("WallF"));
        static_assert(slot(kWork) == 5 && kRoles[slot(kWallC)] == kWallC);
        CHECK(std::ranges::equal(bulbs(), std::span{kRoles}.first(6)));

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

    test("the wall dims away from the sun, in order along the wall", [] {
        Harness h;
        h.tick();
        for (const Role wall : members(Group::Wall)) {
            h.pairBulb(wall, 0xA0 + wall.index, static_cast<uint16_t>(0x0010 + wall.index));
        }
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition = 60s}); // late afternoon at 0°E: the sun is in the west
        std::vector<int> levels;
        for (const auto &t : h.all<LightTarget>()) {
            if (t.role.group == Group::Wall) {
                levels.push_back(t.state.level);
            }
        }
        CHECK(levels.size() == 5);
        CHECK(std::ranges::is_sorted(levels, std::ranges::less{}) && levels.front() < levels.back()); // E brightest
    });

    test("the role manifest is checked against the layout by name", [] {
        struct Entry {
            const char *role;
            uint64_t    ext_addr;
        };
        constexpr std::array entries{Entry{"WallE", 0xE5}, Entry{"work", 0xB2}, Entry{"WallB", 0}};
        constexpr auto manifest = core::DeviceRegistry::manifestOf(entries);
        static_assert(manifest[kWallE] == 0xE5 && manifest[kWork] == 0xB2);
        static_assert(manifest[kWallA] == 0 && manifest[kWallB] == 0); // 0: no entry
        // Wrong names, a role given twice or an address given twice don't compile.

        EventBus          bus;
        core::CoreModules core{bus.post(), manifest};
        std::vector<RoleAssigned> assigned;
        core::wireCore(bus, core);
        bus.on<RoleAssigned>([&](const RoleAssigned &e) { assigned.push_back(e); });
        bus.post()(NetworkUp{});
        bus.post()(DeviceAnnounced{.ext_addr = 0xE5, .short_addr = 0x0005});
        bus.drain();
        CHECK(assigned.size() == 1 && assigned[0].role == kWallE);
    });

    test("at sunset the amber glow sweeps the wall from the west end", [] {
        Harness h;
        TimeReading dusk = noon();
        dusk.local.tm_min = 55; // 17:55 UTC at 0°E: the sun ~2° up, in the west-northwest
        h.tick(1, dusk);
        for (const Role wall : members(Group::Wall)) {
            h.pairBulb(wall, 0xA0 + wall.index, static_cast<uint16_t>(0x0010 + wall.index));
        }
        const auto wallMireds = [&] {
            h.clearLog();
            h.post(ProgramTick{.time = dusk, .transition = 60s});
            std::vector<int> mireds;
            for (const auto &t : h.all<LightTarget>()) {
                if (t.role.group == Group::Wall) {
                    CHECK(t.state.color == ColorMode::Xy);
                    mireds.push_back(t.state.mireds);
                }
            }
            CHECK(mireds.size() == 5);
            return mireds;
        };

        const auto swept = wallMireds();
        CHECK(std::ranges::is_sorted(swept) && swept.front() < swept.back()); // the west end warmest

        ProgramConfig alike;
        alike.amber_sweep_deg = 0;
        h.post(ConfigChanged{.config = alike});
        const auto even = wallMireds();
        CHECK(std::ranges::all_of(even, [&](int m) { return m == even.front(); }));
        CHECK(std::ranges::all_of(swept, [&](int m) { return m <= even.front(); })); // the sweep only holds amber back
        CHECK(even.front() - swept.back() < 10); // and barely at the end nearest the sun
    });

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
