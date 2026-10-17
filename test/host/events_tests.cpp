// The event vocabulary: the bus, attach()'s handler matching, the settings table.

#include "check.h"

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

    test("settings: typed, shown and stored from one table", [] {
        ProgramConfig c;
        const auto    is = [&](std::string_view name, std::string_view text) {
            return show(*findSetting(name), c) == text;
        };
        const auto takes = [&](std::string_view name, std::string_view text) {
            return set(*findSetting(name), c, text).has_value();
        };
        CHECK(is("hours", "08:00-19:00") && is("days", "mon-fri") && is("fade", "10 min"));
        CHECK(is("sunshift", "25%") && is("ambersweep", "3 deg") && is("amber", "on"));

        CHECK((takes("hours", "07:30-24:00") && c.active == DaySpan{7h + 30min, 24h}));
        CHECK(takes("days", "sat,mon") && is("days", "mon,sat"));
        CHECK(takes("clouds", "off") && !c.clouds_enabled);

        const ProgramConfig before = c;
        CHECK(!takes("hours", "19:00-08:00"));    // backwards
        CHECK(!takes("hours", "8-19"));
        CHECK(!takes("sunshift", "101"));
        CHECK(!takes("sunshift", ""));
        CHECK(!takes("ambersweep", "31"));
        CHECK(!takes("fade", "-1"));
        CHECK(!takes("amber", "yes"));
        CHECK(!takes("days", "mon,funday"));
        CHECK(before.active == c.active && before.sun_shift_percent == c.sun_shift_percent); // nothing changed
        CHECK(!findSetting("precip") && !findSetting("alerts")); // declared in the zones instead

        const auto error = set(*findSetting("sunshift"), c, "lots");
        CHECK(!error && error.error() == "sunshift takes 0-100");
        CHECK(!findSetting("color"));

        // The keys the device already has values under.
        const auto keys = settingKeys();
        for (const std::string_view key : {"act.start", "act.end", "act.fade", "act.days", "work.start", "work.end",
                                           "sun.shift", "sun.amber", "sun.sweep", "sky.on", "sky.dim", "sky.cool",
                                           "sync.start", "sync.end", "wx.on"}) {
            CHECK(std::ranges::contains(keys, key));
        }
        CHECK(keys.size() == 15);
    });
}
