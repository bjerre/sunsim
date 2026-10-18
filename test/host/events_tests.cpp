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

void eventsTests()
{
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
}
