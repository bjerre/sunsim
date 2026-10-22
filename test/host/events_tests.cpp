// The event vocabulary: the bus, attach()'s handler matching, the settings table.

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

namespace {
// attach() subscribes a module to exactly the payloads it has `on(const T &)`
// for; a handler that would take one only through a conversion doesn't compile.
struct ExactModule {
    void on(const Tick &) {}
};
struct ByBaseModule {
    void on(const LightChange &) {}
};
struct ByValueModule {
    void on(Tick) {}
};
static_assert(HandlerOf<ExactModule, Tick> && !HandlerOf<ExactModule, ProgramTick>);
static_assert(!HandlerOf<ByBaseModule, LightTarget> && TakesByConversion<ByBaseModule, LightTarget>);
static_assert(!HandlerOf<ByValueModule, Tick> && TakesByConversion<ByValueModule, Tick>);
static_assert(kEventNames[kTypeId<OutputTarget>] == "OutputTarget");
} // namespace

const suite<"events"> eventsTests = [] {
    "the bus delivers every event posted from many threads, in each poster's order"_test = [] {
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
            expect(s.size() == kPerThread && std::ranges::is_sorted(s));
        }
    };

    "settings: typed, shown and stored from one table"_test = [] {
        ProgramConfig c;
        const auto    is = [&](std::string_view name, std::string_view text) {
            return show(*findSetting(name), c) == text;
        };
        const auto takes = [&](std::string_view name, std::string_view text) {
            return set(*findSetting(name), c, text).has_value();
        };
        expect(is("hours", "08:00-19:00") && is("days", "mon-fri") && is("fade", "10 min"));
        expect(is("sunshift", "25%") && is("ambersweep", "3 deg") && is("amber", "on"));

        expect((takes("hours", "07:30-24:00") && c.active == DaySpan{7h + 30min, 24h}));
        expect(takes("days", "sat,mon") && is("days", "mon,sat"));
        expect(takes("clouds", "off") && !c.clouds_enabled);

        const ProgramConfig before = c;
        expect(!takes("hours", "19:00-08:00"));    // backwards
        expect(!takes("hours", "8-19"));
        expect(!takes("sunshift", "101"));
        expect(!takes("sunshift", ""));
        expect(!takes("ambersweep", "31"));
        expect(!takes("fade", "-1"));
        expect(!takes("amber", "yes"));
        expect(!takes("days", "mon,funday"));
        expect(before.active == c.active && before.sun_shift_percent == c.sun_shift_percent); // nothing changed
        expect(!findSetting("precip") && !findSetting("alerts")); // declared in the zones instead

        const auto error = set(*findSetting("sunshift"), c, "lots");
        expect(!error && error.error() == "sunshift takes 0-100");
        expect(!findSetting("color"));

        // The keys the device already has values under.
        const auto keys = settingKeys();
        for (const std::string_view key : {"act.start", "act.end", "act.fade", "act.days", "work.start", "work.end",
                                           "sun.shift", "sun.amber", "sun.sweep", "sky.on", "sky.dim", "sky.cool",
                                           "sync.start", "sync.end", "wx.on"}) {
            expect(std::ranges::contains(keys, key));
        }
        expect(keys.size() == 15);
    };

    "the send queue keeps one command per light, the newer merged over the older"_test = [] {
        SendQueue q;
        q.push(SetLight{.role = kWallA, .power = true, .level = 50, .color = Mireds{300},
                        .level_transition = 1s, .color_transition = 1s});
        q.push(SetLight{.role = kWallA, .level = 60, .level_transition = 60s});
        expect(q.size() == 1uz);
        const auto a = q.pop();
        expect(a && a->power == true && a->level == 60 && a->level_transition == 60s);
        expect(a && colorOf<Mireds>(*a) && colorOf<Mireds>(*a)->value == 300 && a->color_transition == 1s);
        expect(!q.pop() && q.size() == 0uz);

        // Off says it all: the level and color go again with the next On.
        q.push(SetLight{.role = kWallA, .power = true, .level = 50, .color = Mireds{300}});
        q.push(SetLight{.role = kWallA, .power = false});
        const auto off = q.pop();
        expect(off && off->power == false && !off->level && !off->color);

        // On after a waiting Off: the On, with everything it brings.
        q.push(SetLight{.role = kWallA, .power = false});
        q.push(SetLight{.role = kWallA, .power = true, .level = 40, .color = Xy{0.3f, 0.3f}});
        const auto on = q.pop();
        expect(on && on->power == true && on->level == 40 && colorOf<Xy>(*on));
    };

    "the send queue takes the lights in turn, and reports the most waiting"_test = [] {
        SendQueue q;
        for (const Role role : {kWallA, kWallB, kWallC}) {
            q.push(SetLight{.role = role, .level = 10});
        }
        expect(q.pop()->role == kWallA);
        q.push(SetLight{.role = kWallA, .level = 20}); // back of the line, after B and C
        expect(q.pop()->role == kWallB);
        expect(q.pop()->role == kWallC);
        expect(q.pop()->role == kWallA);
        expect(!q.pop());
        expect(q.takeHighWater() == 3uz);
        expect(q.takeHighWater() == 0uz); // since the last report: none
    };
};
