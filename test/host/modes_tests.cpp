// The button and the modes: red alert, all off, click timing, tuning.

#include "check.h"

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

void modesTests()
{
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
        CHECK((red.size() == 2 && colorOf<HueSat>(red[0]) && *colorOf<HueSat>(red[0]) == HueSat{0, 254}));

        h.clearLog();
        h.tick(3);
        const auto pulses = h.all<SetLight>();
        CHECK(pulses.size() == 6);
        CHECK(pulses.size() == 6 && pulses[0].level && pulses[2].level && *pulses[0].level != *pulses[2].level); // bright/dim alternate
        CHECK(pulses.size() == 6 && !pulses[0].color); // already red: only the level moves

        h.clearLog();
        h.post(ButtonPressed{.press = Press::Single, .source = Source::Zigbee, .at = h.now});
        modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight);
        const auto back = h.all<SetLight>();
        CHECK(back.size() == 2 && back[0].level && (colorOf<Mireds>(back[0]) || colorOf<Xy>(back[0]))); // daylight at once

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
        CHECK(lights.size() == 1 && colorOf<HueSat>(lights[0]) && colorOf<HueSat>(lights[0])->hue == 0);
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

    test("an ambiguous press alone resolves as the window ends (a Wake), not at the next Tick", [] {
        Harness h;
        h.tick();
        const Instant pressed = h.now + 100ms;
        h.post(ButtonPressed{.press = Press::Ambiguous, .at = pressed});
        const auto wakes = h.all<WakeAt>();
        CHECK(!wakes.empty() && wakes.back().at == pressed + 450ms); // asked to be woken as the window ends
        CHECK(h.all<ModeChanged>().empty());
        h.post(Wake{.at = pressed + 450ms});
        const auto modes = h.all<ModeChanged>();
        CHECK(modes.size() == 1 && modes[0].mode == Mode::Daylight);
    });

    test("red alert keeps its beat: a second Tick in the same second doesn't flip it", [] {
        Harness h;
        h.tick();
        h.pairBulb(kWallA, 0xA1, 0x0001);
        h.post(ButtonPressed{.press = Press::Double, .at = h.now});
        h.tick();
        h.clearLog();
        h.post(Tick{.at = h.now + 300ms, .time = noon()}); // late, or a duplicate: same second
        CHECK(h.all<SetLight>().empty());
        h.tick();
        CHECK(h.all<SetLight>().size() == 1); // the next second flips it
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

    test("tuning: bulbs at their minimums, `tune` sets levels, save makes them the minimums", [] {
        Harness h;
        h.tick();
        MinLevels mins = kDefaultMinLevels;
        mins[kWork]    = 12;
        h.post(ConfigChanged{.config = {}, .min_levels = mins});
        h.clearLog();

        h.post(ModeRequested{Mode::Tuning});
        const auto start = h.all<LightTarget>();
        CHECK(start.size() == kLightCount);
        for (const auto &t : start) {
            CHECK(t.state.on && colorOf<Xy>(t.state)); // the sunset's amber
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
}
