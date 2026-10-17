// The daylight program: the sun curve along the wall, the clock guard, clouds, minimum levels.

#include "check.h"

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

void daylightTests()
{
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

    test("at sunset the amber glow sweeps the wall from the west end", [] {
        Harness h;
        const TimeReading dusk = at(12h + 55min); // 17:55 UTC at 0°E: the sun ~2° up, in the west-northwest
        h.tick(1, dusk);
        for (const Role wall : members(Group::Wall)) {
            h.pairBulb(wall, 0xA0 + wall.index, static_cast<uint16_t>(0x0010 + wall.index));
        }
        // How amber each wall light is: along the blackbody line, x grows as it warms.
        const auto wallAmber = [&] {
            h.clearLog();
            h.post(ProgramTick{.time = dusk, .transition = 60s});
            std::vector<float> amber;
            for (const auto &t : h.all<LightTarget>()) {
                if (t.role.group == Group::Wall) {
                    CHECK(colorOf<Xy>(t.state));
                    amber.push_back(colorOf<Xy>(t.state) ? colorOf<Xy>(t.state)->x : 0.0f);
                }
            }
            CHECK(amber.size() == 5);
            return amber;
        };

        const auto swept = wallAmber();
        CHECK(std::ranges::is_sorted(swept) && swept.front() < swept.back()); // the west end warmest

        ProgramConfig alike;
        alike.amber_sweep_deg = 0;
        h.post(ConfigChanged{.config = alike});
        const auto even = wallAmber();
        CHECK(std::ranges::all_of(even, [&](float x) { return x == even.front(); }));
        CHECK(std::ranges::all_of(swept, [&](float x) { return x <= even.front(); })); // the sweep only holds amber back
        CHECK(even.front() - swept.back() < 0.01f); // and barely at the end nearest the sun
    });

    test("cloud cover: interpolated by local hour; fast-day reads today's", [] {
        const auto half = core::cloudCover(tuesdayForecast(), at(12h + 30min));
        CHECK(half && std::abs(*half - 0.40) < 1e-9);

        TimeReading tomorrow = noon();
        tomorrow.local += std::chrono::days{1};
        const auto next = core::cloudCover(tuesdayForecast(), tomorrow);
        CHECK(next && std::abs(*next - 0.90) < 1e-9); // hour 36, with no data at 37

        TimeReading sim = noon();
        sim.local       = std::chrono::local_days{2024y / std::chrono::January / 1} + 12h; // fast-day's week
        sim.simulated   = true;
        const auto today = core::cloudCover(tuesdayForecast(), sim);
        CHECK(today && std::abs(*today - 0.20) < 1e-9);

        sim.simulated = false;
        CHECK(!core::cloudCover(tuesdayForecast(), sim));               // a date the forecast doesn't cover
        CHECK(!core::cloudCover(CloudCover{}, noon()));        // no forecast yet
        CHECK(!core::cloudCover(tuesdayForecast(), at(3h)));            // no data for 03:00
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
        overcast.day = kTuesday;
        overcast.percent.fill(100);
        grey.post(CloudCoverUpdated{overcast});
        const auto cloudy = wall(grey);

        CHECK(sunny.size() == 5 && cloudy.size() == 5);
        if (sunny.size() == 5 && cloudy.size() == 5) {
            CHECK(sunny.front().level < sunny.back().level);  // clear: brighter towards the sun
            CHECK(cloudy.front().level == cloudy.back().level); // overcast: diffuse, even
            CHECK(cloudy.back().level < sunny.back().level);    // and dimmer
            CHECK(colorOf<Mireds>(cloudy.back()) && colorOf<Mireds>(sunny.back()) &&
                  colorOf<Mireds>(cloudy.back())->value < colorOf<Mireds>(sunny.back())->value); // and cooler
        }

        ProgramConfig off;
        off.clouds_enabled = false;
        grey.post(ConfigChanged{.config = off});
        const auto ignored = wall(grey);
        CHECK(ignored.size() == 5 && ignored.back().level == sunny.back().level);
    });

    test("minimum levels keep the wall lit down to sunset, then off", [] {
        const TimeReading dusk = at(12h + 55min); // the sun ~2 deg up
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

        const TimeReading night = at(13h + 30min); // after sunset
        CHECK(westLevel(h, night) == 0);
    });
}
