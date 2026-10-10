// Host tests: the real core modules, wired with the same core::wireCore()
// as the firmware, driven by events; a recorder captures everything posted.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "sunsim/core/core.h"

using namespace sunsim;
using namespace sunsim::ev;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

constexpr int64_t kSecond = 1'000'000;

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
    int64_t                  now_us = 0;

    Harness()
    {
        core::wireCore(bus, core);
        // Record everything posted, including platform-bound events.
        recordAll(std::make_index_sequence<std::variant_size_v<Payload>>{});
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
            now_us += kSecond;
            post(Tick{.uptime_us = now_us, .time = time});
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
        h.post(PairRequested{Role::WallC});
        CHECK(h.all<OpenNetwork>().size() == 1);

        h.post(DeviceAnnounced{.ext_addr = 0xAA, .short_addr = 0x1234});
        const auto assigned = h.all<RoleAssigned>();
        CHECK(assigned.size() == 1 && assigned[0].role == Role::WallC && assigned[0].record.ext_addr == 0xAA);
        CHECK(!h.all<PersistRoles>().empty());
        const auto discover = h.all<Discover>();
        CHECK(discover.size() == 1 && discover[0].role == Role::WallC && discover[0].short_addr == 0x1234);

        h.clearLog();
        h.post(EndpointFound{.role = Role::WallC, .endpoint = 11});
        const auto lights = h.all<SetLight>();
        CHECK(lights.size() == 1 && lights[0].short_addr == 0x1234 && lights[0].endpoint == 11);
        CHECK(lights.size() == 1 && lights[0].power && *lights[0].power); // first command switches it on
    });

    test("a device that already holds a role is never claimed by `pair`", [] {
        Harness h;
        h.pairBulb(Role::WallA, 0xA1, 0x0001);
        h.post(PairRequested{Role::WallC});
        h.clearLog();
        h.post(DeviceAnnounced{.ext_addr = 0xA1, .short_addr = 0x0002}); // WallA power-cycles
        const auto assigned = h.all<RoleAssigned>();
        CHECK(assigned.size() == 1 && assigned[0].role == Role::WallA && assigned[0].record.short_addr == 0x0002);
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
        h.pairBulb(Role::WallA, 0xA1, 0x0001);
        h.pairBulb(Role::Work, 0xB2, 0x0002);
        h.clearLog();

        h.post(ButtonPressed{.press = Press::Double, .source = Source::Zigbee, .at_us = h.now_us});
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
        h.post(ButtonPressed{.press = Press::Single, .source = Source::Zigbee, .at_us = h.now_us});
        modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight);
        const auto back = h.all<SetLight>();
        CHECK(back.size() == 2 && back[0].level && !back[0].hue_sat && (back[0].mireds || back[0].xy)); // daylight at once

    });

    test("hold -> all off; the daylight program is paused until resumed", [] {
        Harness h;
        h.tick();
        h.pairBulb(Role::WallB, 0xC3, 0x0003);
        h.clearLog();
        h.post(ButtonPressed{.press = Press::Hold, .source = Source::Console, .at_us = h.now_us});
        const auto off = h.all<SetLight>();
        CHECK(off.size() == 1 && off[0].power && !*off[0].power);

        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition_ds = 600});
        CHECK(h.all<LightTarget>().empty());
        CHECK(h.all<SetLight>().empty());
    });

    test("a bulb paired during red alert joins in red", [] {
        Harness h;
        h.tick();
        h.post(ButtonPressed{.press = Press::Double, .source = Source::Console, .at_us = h.now_us});
        h.clearLog();
        h.pairBulb(Role::WallC, 0xC3, 0x0003);
        const auto lights = h.all<SetLight>();
        CHECK(lights.size() == 1 && lights[0].power && *lights[0].power);
        CHECK(lights.size() == 1 && lights[0].hue_sat && lights[0].hue_sat->first == 0);
    });

    test("ambiguous On/Toggle: two within 450 ms = double, one alone = single", [] {
        Harness h;
        h.tick();
        h.post(ButtonPressed{.press = Press::Ambiguous, .at_us = h.now_us});
        h.post(ButtonPressed{.press = Press::Ambiguous, .at_us = h.now_us + 200'000});
        auto modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::RedAlert);

        Harness g;
        g.tick();
        g.post(ButtonPressed{.press = Press::Ambiguous, .at_us = g.now_us});
        CHECK(g.all<ModeChanged>().empty()); // still waiting for a possible second click
        g.tick();
        modes = g.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight);
    });

    test("clock not set -> lights held off", [] {
        Harness h;
        TimeReading unset = noon();
        unset.valid       = false;
        h.tick(1, unset);
        h.pairBulb(Role::WallA, 0xA1, 0x0001);
        h.clearLog();
        h.post(ProgramTick{.time = unset, .transition_ds = 600});
        const auto targets = h.all<LightTarget>();
        CHECK(targets.size() == ev::kBulbCount);
        for (const auto &t : targets) {
            CHECK(!t.state.on);
        }
    });

    test("intensity scales the target level; ProgramTick ramps over the interval", [] {
        Harness h;
        h.tick();
        Peaks peaks  = kDefaultPeaks;
        peaks[0]     = 50; // WallA at 50%
        h.post(ConfigChanged{.config = {}, .peaks = peaks});
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition_ds = 600});
        const auto targets = h.all<LightTarget>();
        CHECK(targets.size() == ev::kBulbCount);
        if (targets.size() == ev::kBulbCount) {
            CHECK(targets[0].state.on && targets[1].state.on);
            CHECK(targets[0].state.level * 2 <= targets[1].state.level + 2); // WallA ~half of WallB
            CHECK(targets[0].level_transition_ds == 600 && targets[0].color_transition_ds == 600);
        }
    });

    test("liveness: acks -> online; 150 s of silence -> silent", [] {
        Harness h;
        h.tick();
        h.pairBulb(Role::Work, 0xB2, 0x0042);
        h.post(AckReceived{.short_addr = 0x0042, .rssi = -48});
        h.tick();
        auto snaps = h.all<StatusChanged>();
        CHECK(!snaps.empty() && snaps.back().snapshot->devices[indexOf(Role::Work)].link == DeviceLink::Online);
        CHECK(!snaps.empty() && snaps.back().snapshot->devices[indexOf(Role::Work)].rssi == -48);

        h.clearLog();
        h.tick(160);
        snaps = h.all<StatusChanged>();
        CHECK(!snaps.empty() && snaps.back().snapshot->devices[indexOf(Role::Work)].link == DeviceLink::Silent);
        CHECK(h.all<Ping>().size() >= 2); // pinged every 60 s meanwhile
    });

    test("endpoint discovery is retried every 15 s until found", [] {
        Harness h;
        h.post(PairRequested{Role::WallB});
        h.post(DeviceAnnounced{.ext_addr = 0xC3, .short_addr = 0x0003});
        h.post(EndpointFound{.role = Role::WallB, .endpoint = 0}); // no reply
        h.clearLog();
        h.tick(30);
        CHECK(h.all<Discover>().size() == 2);
    });

    test("button pairing binds it once its endpoint is found", [] {
        Harness h;
        h.post(PairRequested{Role::Button});
        h.post(DeviceAnnounced{.ext_addr = 0x18690AFFFE12620EULL, .short_addr = 0x2FED});
        h.clearLog();
        h.post(EndpointFound{.role = Role::Button, .endpoint = 1});
        const auto binds = h.all<Bind>();
        CHECK(binds.size() == 1 && binds[0].short_addr == 0x2FED && binds[0].endpoint == 1);
        CHECK(h.all<SetLight>().empty()); // never treated as a bulb
    });

    test("a missing bulb is paused, the others keep going, and it resumes when it answers", [] {
        Harness h;
        h.tick();
        h.pairBulb(Role::WallA, 0xA1, 0x0001);
        h.pairBulb(Role::Work, 0xB2, 0x0002); // never answers
        h.post(AckReceived{.short_addr = 0x0001, .rssi = -50});
        h.clearLog();

        h.tick(14);
        h.post(AckReceived{.short_addr = 0x0001, .rssi = -50});
        h.tick(1); // 15 s after boot: first pings
        const auto pings = h.all<Ping>();
        CHECK(pings.size() == 2);

        // WallA's ping is lost too, but it acked recently: still reachable.
        h.post(PingResult{.role = Role::WallA, .ok = false});
        // Work's ping fails and it was never heard after pairing... except
        // at pairing itself, so it needs 60 s of silence first.
        h.post(PingResult{.role = Role::Work, .ok = false});
        CHECK(h.all<ReachabilityChanged>().empty());

        h.tick(60);
        h.post(AckReceived{.short_addr = 0x0001, .rssi = -50});
        h.post(PingResult{.role = Role::Work, .ok = false});
        auto changes = h.all<ReachabilityChanged>();
        CHECK(changes.size() == 1 && changes[0].role == Role::Work && !changes[0].reachable);

        h.clearLog();
        TimeReading later = noon();
        later.local.tm_hour = 14; // the curve has moved on since the last update
        h.post(ProgramTick{.time = later, .transition_ds = 600});
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
        h.post(PingResult{.role = Role::Work, .ok = true}); // powered back on
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
        h.pairBulb(Role::WallB, 0xC3, 0x0003);
        h.post(ProgramTick{.time = noon(), .transition_ds = 600});
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition_ds = 600}); // same time -> same target
        CHECK(h.all<SetLight>().empty());
    });

    test("unpair all clears every role and persists", [] {
        Harness h;
        h.pairBulb(Role::WallA, 0xA1, 0x0001);
        h.clearLog();
        h.post(UnpairRequested{});
        CHECK(h.all<RoleCleared>().size() == ev::kRoleCount);
        const auto persisted = h.all<PersistRoles>();
        CHECK(persisted.size() == 1 && !persisted[0].records[0].valid);
    });

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
