// The weather: reading the services' replies, SkyModel, and the weather lights.

#include "check.h"

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.weather;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

void weatherTests()
{
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

    test("the sky: live cloud cover while fresh, then the forecast; alerts until they end", [&] {
        Harness    h;
        const auto sky = [&] { return h.all<SkyChanged>().back().sky; };
        h.tick();
        h.post(CloudCoverUpdated{tuesdayForecast()});
        CHECK(sky().cloud_percent == 20 && !sky().cloud_live);

        const Instant fetched = h.now;
        h.post(ConditionsUpdated{.now = {.cloud_percent = 90, .precip = Precip::Rain}, .at = fetched});
        CHECK(sky().cloud_percent == 90 && sky().cloud_live && sky().conditions->precip == Precip::Rain);

        AlertsUpdated alerts{.at = fetched};
        alerts.alerts.items[0] = {.level = AlertLevel::Watch, .until = fetched + 30min};
        alerts.alerts.count    = 1;
        h.post(alerts);
        CHECK(sky().alert() == AlertLevel::Watch);

        TimeReading sim = noon();
        sim.simulated   = true; // fast-day plays the forecast's day
        h.tick(1, sim);
        CHECK(sky().cloud_percent == 20 && !sky().cloud_live && sky().conditions);
        h.tick();

        h.clearLog();
        h.now = fetched + 30min - 1s;
        h.tick(); // the watch has ended
        CHECK(h.all<SkyChanged>().size() == 1 && sky().alert() == AlertLevel::None && sky().cloud_live);

        h.now = fetched + kWeatherFreshFor - 1s;
        h.tick(); // the live reading is stale
        CHECK(sky().cloud_percent == 20 && !sky().cloud_live && !sky().conditions);

        h.clearLog();
        h.tick(5);
        CHECK(h.all<SkyChanged>().empty()); // only when it changes
    });

    test("snow pulses the precipitation light, which goes back to the sun after", [&] {
        Harness h;
        weatherRig(h, noon());
        h.post(snowing(h.now));
        h.tick(6);

        const auto c = outputs(h, kWallC);
        CHECK(c.size() >= 3);
        CHECK(std::ranges::all_of(c, [](const OutputTarget &t) {
            return t.state.on && colorOf<HueSat>(t.state) && colorOf<HueSat>(t.state)->saturation == 150;
        }));
        CHECK(std::ranges::any_of(c, [&](const OutputTarget &t) { return t.state.level != c.front().state.level; }));
        CHECK(outputs(h, kWallA).empty()); // the rest of the wall follows the sun alone

        // A program update for WallC meanwhile is held back.
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        CHECK(outputs(h, kWallC).empty() && !outputs(h, kWallA).empty());

        h.clearLog();
        h.post(ConditionsUpdated{.now = {.cloud_percent = 100}, .at = h.now}); // let go at once
        const auto back = outputs(h, kWallC);
        CHECK(back.size() == 1 && !colorOf<HueSat>(back[0].state) && back[0].level_transition == 2s);
    });

    test("snow outside active hours glows dimmer; not at all in off mode", [&] {
        const TimeReading night = at(22h);

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
        const TimeReading night = at(22h);
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
            CHECK(alarm.size() == 5); // at once, then a fade every second
            CHECK(std::ranges::all_of(alarm, [](const OutputTarget &t) {
                return t.state.on && colorOf<HueSat>(t.state) && colorOf<HueSat>(t.state)->hue == 0;
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
        CHECK(!c.empty() && std::ranges::all_of(c, [](const OutputTarget &t) { return colorOf<HueSat>(t.state) && colorOf<HueSat>(t.state)->hue == 35; }));

        both.weather_enabled = false;
        h.post(ConfigChanged{.config = both});
        h.clearLog();
        h.tick(3);
        const auto plain = outputs(h, kWallC);
        CHECK(!plain.empty() && std::ranges::none_of(plain, [](const OutputTarget &t) {
            return colorOf<HueSat>(t.state) != nullptr;
        }));
    });
}
