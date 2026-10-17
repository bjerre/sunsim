// Host tests: the real core modules, wired with the same core::wireCore()
// as the firmware, driven by events; a recorder captures everything posted.

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.weather;

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

    // A forecast for noon()'s date (tm_year 0 = 1900, yday 171): 20% at
    // 12:00, 60% at 13:00, the next day's noon 90%.
    const auto forecast = [] {
        CloudCover f;
        f.year       = 1900;
        f.yday       = 171;
        f.percent[12] = 20;
        f.percent[13] = 60;
        f.percent[36] = 90;
        return f;
    }();

    test("cloud cover: interpolated by local hour; fast-day reads today's", [&] {
        TimeReading t = noon();
        t.local.tm_min = 30;
        const auto half = core::cloudCover(forecast, t);
        CHECK(half && std::abs(*half - 0.40) < 1e-9);

        TimeReading tomorrow = noon();
        tomorrow.local.tm_yday = 172;
        const auto next = core::cloudCover(forecast, tomorrow);
        CHECK(next && std::abs(*next - 0.90) < 1e-9); // hour 36, with no data at 37

        TimeReading sim = noon();
        sim.local.tm_yday = 0; // fast-day's January 1
        sim.simulated     = true;
        const auto today = core::cloudCover(forecast, sim);
        CHECK(today && std::abs(*today - 0.20) < 1e-9);

        sim.simulated = false;
        CHECK(!core::cloudCover(forecast, sim));               // a date the forecast doesn't cover
        CHECK(!core::cloudCover(CloudCover{}, noon()));        // no forecast yet
        TimeReading early = noon();
        early.local.tm_hour = 3;
        CHECK(!core::cloudCover(forecast, early));             // no data for 03:00
    });

    test("clouds dim the wall, cool it and flatten its sun direction", [] {
        const auto wall = [](Harness &h) {
            h.clearLog();
            h.post(ProgramTick{.time = noon(), .transition = 60s});
            std::vector<LightState> states;
            for (const auto &t : h.all<LightTarget>()) {
                if (t.role.group == Group::Wall) {
                    states.push_back(t.state);
                }
            }
            return states;
        };
        Harness clear;
        clear.tick();
        const auto sunny = wall(clear);

        Harness grey;
        grey.tick();
        CloudCover overcast;
        overcast.year = 1900;
        overcast.yday = 171;
        overcast.percent.fill(100);
        grey.post(CloudCoverUpdated{overcast});
        const auto cloudy = wall(grey);

        CHECK(sunny.size() == 5 && cloudy.size() == 5);
        if (sunny.size() == 5 && cloudy.size() == 5) {
            CHECK(sunny.front().level < sunny.back().level);  // clear: brighter towards the sun
            CHECK(cloudy.front().level == cloudy.back().level); // overcast: diffuse, even
            CHECK(cloudy.back().level < sunny.back().level);    // and dimmer
            CHECK(cloudy.back().mireds < sunny.back().mireds);  // and cooler
        }

        ProgramConfig off;
        off.clouds_enabled = false;
        grey.post(ConfigChanged{.config = off});
        const auto ignored = wall(grey);
        CHECK(ignored.size() == 5 && ignored.back().level == sunny.back().level);
    });

    test("minimum levels keep the wall lit down to sunset, then off", [] {
        TimeReading dusk = noon();
        dusk.local.tm_min = 55; // the sun ~2 deg up
        const auto westLevel = [](Harness &h, const TimeReading &t) {
            h.clearLog();
            h.post(ProgramTick{.time = t, .transition = 60s});
            const auto targets = h.all<LightTarget>();
            const auto it      = std::ranges::find(targets, kWallE, &LightTarget::role);
            return it == targets.end() ? -1 : (it->state.on ? it->state.level : 0);
        };
        Harness h;
        h.tick();
        const int dim = westLevel(h, dusk);

        MinLevels mins = kDefaultMinLevels;
        mins[kWallE]   = 40;
        h.post(ConfigChanged{.config = {}, .min_levels = mins});
        const int floored = westLevel(h, dusk);
        CHECK(dim >= 1 && dim < 40);
        CHECK(floored >= 40);

        TimeReading night = noon();
        night.local.tm_hour = 13;
        night.local.tm_min  = 30; // after sunset
        CHECK(westLevel(h, night) == 0);
    });

    test("tuning: bulbs at their minimums, `tune` sets levels, save makes them the minimums", [] {
        Harness h;
        h.tick();
        MinLevels mins = kDefaultMinLevels;
        mins[kWork]    = 12;
        h.post(ConfigChanged{.config = {}, .min_levels = mins});
        h.clearLog();

        h.post(ModeRequested{Mode::Tuning});
        const auto start = h.all<LightTarget>();
        CHECK(start.size() == kBulbCount);
        for (const auto &t : start) {
            CHECK(t.state.on && t.state.color == ColorMode::Xy); // the sunset's amber
            CHECK(t.state.level == (t.role == kWork ? 12 : 1));
        }

        h.clearLog();
        h.post(TuneLights{.lights = Group::Wall, .level = 9});
        for (const auto &t : h.all<LightTarget>()) {
            CHECK(t.state.level == (t.role.group == Group::Wall ? 9 : 12));
        }

        h.clearLog();
        h.post(TuneSave{});
        const auto saved = h.all<SetMinLevel>();
        CHECK(saved.size() == members(Group::Wall).size());
        CHECK(std::ranges::all_of(saved, [](const SetMinLevel &s) {
            return s.level == 9 && std::holds_alternative<Role>(s.lights) && std::get<Role>(s.lights).group == Group::Wall;
        }));

        h.clearLog();
        h.post(ButtonPressed{.press = Press::Single, .at = h.now});
        const auto modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight); // daylight, sky and all
    });

    // ---- Weather: reading the services' replies -------------------------------

    test("the JSON scanner: keys, parents and escapes, whatever the chunk boundaries", [] {
        const std::string bs(1, '\\'); // spelled out: the compiler reads a \u escape even in a raw string
        const std::string json = R"({"a": {"b": 1.5, "c": [10, null, "x\"y"]}, "d": [{"e": true}], "f": ")" + bs +
                                 "u00e9t" + bs + R"(u00e9"})";
        for (const std::size_t chunk : {1uz, 3uz, json.size()}) {
            std::vector<std::string> seen;
            std::vector<std::string> closed;
            weather::JsonScanner     scanner{
                [&](const weather::JsonScanner::Scalar &s) {
                    seen.push_back(std::string{s.parent} + "/" + std::string{s.key} + "=" + std::string{s.text});
                },
                [&](std::string_view key) { closed.emplace_back(key); }};
            for (std::size_t i = 0; i < json.size(); i += chunk) {
                scanner.feed(std::string_view{json}.substr(i, chunk));
            }
            CHECK((seen == std::vector<std::string>{"a/b=1.5", "a/c=10", "a/c=null", "a/c=x\"y", "d/e=true", "/f=?t?"}));
            CHECK((closed == std::vector<std::string>{"a", "d", ""}));
        }
    });

    test("Open-Meteo's current conditions: cloud and what's falling, units skipped", [] {
        constexpr std::string_view reply =
            R"({"latitude":44.98,"longitude":-93.27,"current_units":{"time":"unixtime","interval":"seconds",)"
            R"("cloud_cover":"%","weather_code":"wmo code"},"current":{"time":1760629500,"interval":900,)"
            R"("cloud_cover":87,"weather_code":73}})";
        weather::ConditionsReader reader;
        for (std::size_t i = 0; i < reply.size(); i += 5) {
            reader.feed(reply.substr(i, 5));
        }
        const auto now = reader.conditions();
        CHECK(now && now->cloud_percent == 87 && now->precip == Precip::Snow && now->intensity == Intensity::Moderate);

        weather::ConditionsReader empty;
        empty.feed(R"({"error":true,"reason":"bad"})");
        CHECK(!empty.conditions());

        CHECK(weather::precipOf(3).first == Precip::None);  // overcast
        CHECK(weather::precipOf(45).first == Precip::None); // fog
        CHECK((weather::precipOf(82) == std::pair{Precip::Rain, Intensity::Heavy}));
        CHECK(weather::precipOf(67).first == Precip::FreezingRain);
        CHECK(weather::precipOf(99).first == Precip::Thunderstorm);
    });

    test("Open-Meteo's hourly forecast: times paired with cover, nulls dropped", [] {
        weather::ForecastReader reader;
        reader.feed(R"({"hourly_units":{"time":"unixtime","cloud_cover":"%"},)"
                    R"("hourly":{"time":[1760572800,1760576400,1760580000],"cloud_cover":[12,null,100]}})");
        const auto hours = reader.hours();
        CHECK(hours.size() == 2);
        CHECK(hours[0].start.time_since_epoch().count() == 1760572800 && hours[0].cloud_percent == 12);
        CHECK(hours[1].start.time_since_epoch().count() == 1760580000 && hours[1].cloud_percent == 100);
    });

    test("NWS times: offsets and Z", [] {
        const auto t = weather::parseIsoTime("2026-10-16T18:00:00-05:00");
        const auto z = weather::parseIsoTime("2026-10-16T23:00:00Z");
        CHECK(t && z && *t == *z);
        CHECK(weather::parseIsoTime("2026-10-17T01:30:00+02:30") == weather::parseIsoTime("2026-10-16T23:00:00Z"));
        CHECK(!weather::parseIsoTime("soon"));
        CHECK(!weather::parseIsoTime("2026-13-01T00:00:00Z"));
    });

    test("NWS alerts: tiers by name, one per event, cancellations and statements dropped", [] {
        // As api.weather.gov sends it: pretty-printed GeoJSON, nested
        // references and parameters, long texts, a null `ends`.
        constexpr std::string_view reply = R"({
    "@context": ["https://geojson.org/geojson-ld/geojson-context.jsonld", {"@version": "1.1"}],
    "type": "FeatureCollection",
    "features": [
        {
            "id": "https://api.weather.gov/alerts/urn:oid:1",
            "type": "Feature",
            "geometry": {"type": "Polygon", "coordinates": [[[-93.1, 44.9], [-93.0, 45.0]]]},
            "properties": {
                "id": "urn:oid:1",
                "areaDesc": "Hennepin",
                "geocode": {"SAME": ["027053"], "UGC": ["MNZ060"]},
                "references": [{"@id": "x", "identifier": "urn:oid:0", "sent": "2026-10-16T08:00:00-05:00"}],
                "sent": "2026-10-16T10:00:00-05:00",
                "expires": "2026-10-16T16:00:00-05:00",
                "ends": "2026-10-17T06:00:00-05:00",
                "status": "Actual",
                "messageType": "Update",
                "severity": "Moderate",
                "event": "Winter Storm Watch",
                "headline": "Winter Storm Watch issued October 16",
                "description": "* WHAT...Heavy snow possible. Total \"snow\" accumulations of 8 to 12 inches.\n\n* WHERE...Hennepin.",
                "parameters": {"NWSheadline": ["WINTER STORM WATCH IN EFFECT"], "event": ["ignored"]}
            }
        },
        {
            "properties": {"status": "Actual", "messageType": "Alert", "severity": "Severe",
                           "event": "Winter Storm Watch", "expires": "2026-10-16T12:00:00-05:00", "ends": null}
        },
        {
            "properties": {"status": "Actual", "messageType": "Alert", "severity": "Severe",
                           "event": "Tornado Warning", "ends": "2026-10-16T11:45:00-05:00"}
        },
        {
            "properties": {"status": "Actual", "messageType": "Cancel", "severity": "Minor",
                           "event": "Wind Advisory", "ends": "2026-10-16T20:00:00-05:00"}
        },
        {
            "properties": {"status": "Actual", "messageType": "Alert", "severity": "Minor",
                           "event": "Special Weather Statement", "ends": "2026-10-16T20:00:00-05:00"}
        },
        {
            "properties": {"status": "Test", "messageType": "Alert", "severity": "Extreme",
                           "event": "Tornado Warning", "ends": "2026-10-16T20:00:00-05:00"}
        }
    ],
    "title": "Current watches, warnings, and advisories",
    "updated": "2026-10-16T10:05:00+00:00"
})";
        weather::AlertsReader reader;
        for (std::size_t i = 0; i < reply.size(); i += 7) {
            reader.feed(reply.substr(i, 7));
        }
        const auto &all = reader.alerts();
        CHECK(all.size() == 3); // the watch twice, the warning
        CHECK(all.size() == 3 && all[0].event == "Winter Storm Watch" && all[0].level == AlertLevel::Watch);
        CHECK(all.size() == 3 && all[2].level == AlertLevel::Warning);

        const auto wall_now = *weather::parseIsoTime("2026-10-16T11:00:00-05:00");
        const Instant boot_now{100s};
        const Alerts in_force = reader.inForce(wall_now, boot_now);
        CHECK(in_force.count == 2);
        CHECK(std::string_view{in_force.items[0].event.data()} == "Tornado Warning");
        CHECK(in_force.items[0].until == boot_now + 45min);
        CHECK(std::string_view{in_force.items[1].event.data()} == "Winter Storm Watch");
        CHECK(in_force.items[1].until == boot_now + 19h); // the update's end, not the original's

        CHECK(reader.inForce(wall_now + 1h, boot_now).count == 1); // the warning has ended

        CHECK(weather::alertLevelOf("Tornado Emergency", "Extreme") == AlertLevel::Warning);
        CHECK(weather::alertLevelOf("Heat Advisory", "Moderate") == AlertLevel::Advisory);
        CHECK(weather::alertLevelOf("Hazardous Weather Outlook", "Minor") == AlertLevel::None);
    });

    // ---- Weather: the live sky and the weather lights ---------------------------

    test("live cloud cover wins while fresh, then the forecast again", [&] {
        core::Weather w;
        w.onForecast(CloudCoverUpdated{forecast});
        const Instant fetched{1h};
        w.onConditions(ConditionsUpdated{.now = {.cloud_percent = 90}, .at = fetched});

        const auto live = w.cloud(noon(), fetched + 10min);
        CHECK(live && live->live && std::abs(live->cover - 0.90) < 1e-9);

        const auto stale = w.cloud(noon(), fetched + kWeatherFreshFor);
        CHECK(stale && !stale->live && std::abs(stale->cover - 0.20) < 1e-9);

        TimeReading sim = noon();
        sim.simulated   = true; // fast-day plays the forecast's day
        const auto fast = w.cloud(sim, fetched + 10min);
        CHECK(fast && !fast->live);
    });

    test("role sets: lists, groups and none", [] {
        CHECK(parseRoleSet("WallB,walld") == roleSet({kWallB, Role{Group::Wall, 3}}));
        CHECK(parseRoleSet("wall,Work")->count() == kBulbCount);
        CHECK(parseRoleSet("none")->none());
        CHECK(!parseRoleSet("WallB,attic"));
    });

    // The program's base targets, then the weather's: the harness ticks on.
    const auto weatherRig = [](Harness &h, TimeReading time) {
        h.tick(1, time);
        h.post(ProgramTick{.time = time, .transition = 60s});
        h.clearLog();
    };
    const auto outputs = [](const Harness &h, Role role) {
        std::vector<OutputTarget> out;
        for (const auto &t : h.all<OutputTarget>()) {
            if (t.role == role) {
                out.push_back(t);
            }
        }
        return out;
    };
    const auto snowing = [](Instant at, Intensity intensity = Intensity::Moderate) {
        return ConditionsUpdated{.now = {.cloud_percent = 100, .precip = Precip::Snow, .intensity = intensity}, .at = at};
    };
    const auto alerted = [](Instant at, AlertLevel level) {
        AlertsUpdated a{.at = at};
        a.alerts.items[0] = {.level = level, .event = {'X'}, .until = at + 2h};
        a.alerts.count    = 1;
        return a;
    };

    test("snow pulses the precipitation light, which goes back to the sun after", [&] {
        Harness h;
        weatherRig(h, noon());
        h.post(snowing(h.now));
        h.tick(6);

        const auto c = outputs(h, kWallC);
        CHECK(c.size() >= 3);
        CHECK(std::ranges::all_of(c, [](const OutputTarget &t) {
            return t.state.on && t.state.color == ColorMode::HueSat && t.state.saturation == 150;
        }));
        CHECK(std::ranges::any_of(c, [&](const OutputTarget &t) { return t.state.level != c.front().state.level; }));
        CHECK(outputs(h, kWallA).empty()); // the rest of the wall follows the sun alone

        // A program update for WallC meanwhile is held back.
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        CHECK(outputs(h, kWallC).empty() && !outputs(h, kWallA).empty());

        h.post(ConditionsUpdated{.now = {.cloud_percent = 100}, .at = h.now});
        h.clearLog();
        h.tick();
        const auto back = outputs(h, kWallC);
        CHECK(back.size() == 1 && back[0].state.color != ColorMode::HueSat && back[0].level_transition == 2s);
    });

    test("snow outside active hours glows dimmer; not at all in off mode", [&] {
        TimeReading night = noon();
        night.local.tm_hour = 22;

        Harness lit;
        weatherRig(lit, noon());
        lit.post(snowing(lit.now, Intensity::Heavy));
        lit.tick(6, noon());
        Harness dark;
        weatherRig(dark, night);
        dark.post(snowing(dark.now, Intensity::Heavy));
        dark.tick(6, night);

        const auto top = [&](const Harness &h) {
            uint8_t level = 0;
            for (const auto &t : outputs(h, kWallC)) {
                level = std::max(level, t.state.level);
            }
            return level;
        };
        CHECK(top(lit) == kMaxLevel);
        CHECK(top(dark) < kMaxLevel / 2 && top(dark) > 0);

        dark.post(ButtonPressed{.press = Press::Hold, .at = dark.now}); // all off
        dark.clearLog();
        dark.tick(6, night);
        const auto off = outputs(dark, kWallC);
        CHECK(std::ranges::all_of(off, [](const OutputTarget &t) { return !t.state.on; }));
    });

    test("a warning pulses red even with the lights off; a watch only on lit lights", [&] {
        TimeReading night = noon();
        night.local.tm_hour = 22;
        Harness h;
        weatherRig(h, night);
        h.post(ButtonPressed{.press = Press::Hold, .at = h.now}); // all off
        h.post(alerted(h.now, AlertLevel::Watch));
        h.clearLog();
        h.tick(4, night);
        CHECK(outputs(h, kWallB).empty());

        h.post(alerted(h.now, AlertLevel::Warning));
        h.tick(4, night);
        for (const Role role : {kWallB, Role{Group::Wall, 3}}) {
            const auto alarm = outputs(h, role);
            CHECK(alarm.size() == 4); // a fade every second
            CHECK(std::ranges::all_of(alarm, [](const OutputTarget &t) {
                return t.state.on && t.state.color == ColorMode::HueSat && t.state.hue == 0;
            }));
        }
        CHECK(outputs(h, kWallC).empty());

        h.clearLog();
        h.tick(1, night);
        h.now += kWeatherFreshFor; // no news since: stale
        h.tick(1, night);
        const auto released = outputs(h, kWallB);
        CHECK(!released.empty() && !released.back().state.on);
    });

    test("an alert outranks the precipitation on a light that's both; weather off stops both", [&] {
        Harness h;
        weatherRig(h, noon());
        ProgramConfig both;
        both.alert_lights = both.precip_lights = roleSet({kWallC});
        h.post(ConfigChanged{.config = both});
        h.post(snowing(h.now));
        h.post(alerted(h.now, AlertLevel::Advisory));
        h.clearLog();
        h.tick(3);
        const auto c = outputs(h, kWallC);
        CHECK(!c.empty() && std::ranges::all_of(c, [](const OutputTarget &t) { return t.state.hue == 35; }));

        both.weather_enabled = false;
        h.post(ConfigChanged{.config = both});
        h.clearLog();
        h.tick(3);
        const auto plain = outputs(h, kWallC);
        CHECK(!plain.empty() && std::ranges::none_of(plain, [](const OutputTarget &t) {
            return t.state.color == ColorMode::HueSat;
        }));
    });

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
