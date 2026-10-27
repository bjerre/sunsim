// The weather: reading the services' replies, SkyModel, and the weather lights.

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.weather;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

const suite<"weather"> weatherTests = [] {
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

    "the JSON scanner: keys, parents and escapes, whatever the chunk boundaries"_test = [] {
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
            expect((seen == std::vector<std::string>{"a/b=1.5", "a/c=10", "a/c=null", "a/c=x\"y", "d/e=true", "/f=?t?"}));
            expect((closed == std::vector<std::string>{"a", "d", ""}));
        }
    };

    "JSON scalar numbers parse only complete decimal numbers"_test = [] {
        const auto number = [](std::string_view text) {
            weather::JsonScanner::Scalar scalar;
            scalar.text = text;
            return scalar.number();
        };
        expect(number("-12.5e+2") == -1250.0);
        expect(number("0") == 0.0);
        expect(!number(""));
        expect(number("01") == 1.0);
        expect(number("1.") == 1.0);
        expect(!number("1x"));
        expect(!number("0x1p2"));
        weather::JsonScanner::Scalar string_number;
        string_number.text = "42";
        string_number.is_string = true;
        expect(!string_number.number());
    };

    "Open-Meteo's current conditions: cloud and what's falling, units skipped"_test = [] {
        constexpr std::string_view reply =
            R"({"latitude":44.98,"longitude":-93.27,"current_units":{"time":"unixtime","interval":"seconds",)"
            R"("cloud_cover":"%","weather_code":"wmo code"},"current":{"time":1760629500,"interval":900,)"
            R"("cloud_cover":87,"weather_code":73}})";
        weather::ConditionsReader reader;
        for (std::size_t i = 0; i < reply.size(); i += 5) {
            reader.feed(reply.substr(i, 5));
        }
        const auto now = reader.conditions();
        expect(now && now->cloud_percent == 87 && now->precip == Precip::Snow && now->intensity == Intensity::Moderate);

        weather::ConditionsReader empty;
        empty.feed(R"({"error":true,"reason":"bad"})");
        expect(!empty.conditions());

        expect(weather::precipOf(3).first == Precip::None);  // overcast
        expect(weather::precipOf(45).first == Precip::None); // fog
        expect((weather::precipOf(82) == std::pair{Precip::Rain, Intensity::Heavy}));
        expect(weather::precipOf(67).first == Precip::FreezingRain);
        expect(weather::precipOf(99).first == Precip::Thunderstorm);
    };

    "Open-Meteo's hourly forecast: times paired with cover, nulls dropped"_test = [] {
        weather::ForecastReader reader;
        reader.feed(R"({"hourly_units":{"time":"unixtime","cloud_cover":"%"},)"
                    R"("hourly":{"time":[1760572800,1760576400,1760580000],"cloud_cover":[12,null,100]}})");
        const auto hours = reader.hours();
        expect(hours.size() == 2);
        expect(hours[0].start.time_since_epoch().count() == 1760572800 && hours[0].cloud_percent == 12);
        expect(hours[1].start.time_since_epoch().count() == 1760580000 && hours[1].cloud_percent == 100);
    };

    "NWS times: offsets and Z"_test = [] {
        const auto t = weather::parseIsoTime("2026-10-16T18:00:00-05:00");
        const auto z = weather::parseIsoTime("2026-10-16T23:00:00Z");
        expect(t && z && *t == *z);
        expect(weather::parseIsoTime("2026-10-17T01:30:00+02:30") == weather::parseIsoTime("2026-10-16T23:00:00Z"));
        expect(!weather::parseIsoTime("soon"));
        expect(!weather::parseIsoTime("2026-13-01T00:00:00Z"));
    };

    "NWS alerts: tiers by name, one per event, cancellations and statements dropped"_test = [] {
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
        expect(all.size() == 3); // the watch twice, the warning
        expect(all.size() == 3 && all[0].event == "Winter Storm Watch" && all[0].level == AlertLevel::Watch);
        expect(all.size() == 3 && all[2].level == AlertLevel::Warning);

        const auto wall_now = *weather::parseIsoTime("2026-10-16T11:00:00-05:00");
        const Instant boot_now{100s};
        const Alerts in_force = reader.inForce(wall_now, boot_now);
        expect(in_force.count == 2);
        expect(std::string_view{in_force.items[0].event.data()} == "Tornado Warning");
        expect(in_force.items[0].until == boot_now + 45min);
        expect(std::string_view{in_force.items[1].event.data()} == "Winter Storm Watch");
        expect(in_force.items[1].until == boot_now + 19h); // the update's end, not the original's

        expect(reader.inForce(wall_now + 1h, boot_now).count == 1); // the warning has ended

        expect(weather::alertLevelOf("Tornado Emergency", "Extreme") == AlertLevel::Warning);
        expect(weather::alertLevelOf("Heat Advisory", "Moderate") == AlertLevel::Advisory);
        expect(weather::alertLevelOf("Hazardous Weather Outlook", "Minor") == AlertLevel::None);
    };

    "the sky: live cloud cover while fresh, then the forecast; alerts until they end"_test = [&] {
        Harness    h;
        const auto sky = [&] { return h.all<SkyChanged>().back().sky; };
        h.tick();
        h.post(CloudCoverUpdated{tuesdayForecast()});
        expect(sky().cloud_percent == 20 && !sky().cloud_live);

        const Instant fetched = h.now;
        h.post(ConditionsUpdated{.now = {.cloud_percent = 90, .precip = Precip::Rain}, .at = fetched});
        expect(sky().cloud_percent == 90 && sky().cloud_live && sky().conditions->precip == Precip::Rain);

        AlertsUpdated alerts{.at = fetched};
        alerts.alerts.items[0] = {.level = AlertLevel::Watch, .until = fetched + 30min};
        alerts.alerts.count    = 1;
        h.post(alerts);
        expect(sky().alert() == AlertLevel::Watch);

        TimeReading sim = noon();
        sim.simulated   = true; // fast-day plays the forecast's day
        h.tick(1, sim);
        expect(sky().cloud_percent == 20 && !sky().cloud_live && sky().conditions);
        h.tick();

        h.clearLog();
        h.now = fetched + 30min - 1s;
        h.tick(); // the watch has ended
        expect(h.all<SkyChanged>().size() == 1 && sky().alert() == AlertLevel::None && sky().cloud_live);

        h.now = fetched + kWeatherFreshFor - 1s;
        h.tick(); // the live reading is stale
        expect(sky().cloud_percent == 20 && !sky().cloud_live && !sky().conditions);

        h.clearLog();
        h.tick(5);
        expect(h.all<SkyChanged>().empty()); // only when it changes
    };

    "snow pulses the precipitation light, which goes back to the sun after"_test = [&] {
        Harness h;
        weatherRig(h, noon());
        h.post(snowing(h.now));
        h.tick(6);

        const auto c = outputs(h, kWallC);
        expect(c.size() >= 3);
        expect(std::ranges::all_of(c, [](const OutputTarget &t) {
            return t.state.on && colorOf<HueSat>(t.state) && colorOf<HueSat>(t.state)->saturation == 150;
        }));
        expect(std::ranges::any_of(c, [&](const OutputTarget &t) { return t.state.level != c.front().state.level; }));
        expect(outputs(h, kWallA).empty()); // the rest of the wall follows the sun alone

        // A program update for WallC meanwhile is held back.
        h.clearLog();
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        expect(outputs(h, kWallC).empty() && !outputs(h, kWallA).empty());

        h.clearLog();
        h.post(ConditionsUpdated{.now = {.cloud_percent = 100}, .at = h.now}); // let go at once
        const auto back = outputs(h, kWallC);
        expect(back.size() == 1 && !colorOf<HueSat>(back[0].state) && back[0].level_transition == 2s);
    };

    "snow outside active hours glows dimmer; not at all in off mode"_test = [&] {
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
        expect(top(lit) == kMaxLevel);
        expect(top(dark) < kMaxLevel / 2 && top(dark) > 0);

        dark.post(ButtonPressed{.press = Press::Hold, .at = dark.now}); // all off
        dark.clearLog();
        dark.tick(6, night);
        const auto off = outputs(dark, kWallC);
        expect(std::ranges::all_of(off, [](const OutputTarget &t) { return !t.state.on; }));
    };

    "a warning pulses red even with the lights off; a watch only on lit lights"_test = [&] {
        const TimeReading night = at(22h);
        Harness h;
        weatherRig(h, night);
        h.post(ButtonPressed{.press = Press::Hold, .at = h.now}); // all off
        h.post(alerted(h.now, AlertLevel::Watch));
        h.clearLog();
        h.tick(4, night);
        expect(outputs(h, kWallB).empty());

        h.post(alerted(h.now, AlertLevel::Warning));
        h.tick(4, night);
        for (const Role role : {kWallB, Role{Group::Wall, 3}}) {
            const auto alarm = outputs(h, role);
            expect(alarm.size() == 5); // at once, then a fade every second
            expect(std::ranges::all_of(alarm, [](const OutputTarget &t) {
                return t.state.on && colorOf<HueSat>(t.state) && colorOf<HueSat>(t.state)->hue == 0;
            }));
        }
        expect(outputs(h, kWallC).empty());

        h.clearLog();
        h.tick(1, night);
        h.now += kWeatherFreshFor; // no news since: stale
        h.tick(1, night);
        const auto released = outputs(h, kWallB);
        expect(!released.empty() && !released.back().state.on);
    };

    "an alert outranks the precipitation on a light that's both; weather off stops both"_test = [] {
        Sky sky;
        sky.conditions      = Conditions{.cloud_percent = 100, .precip = Precip::Snow};
        sky.alerts.items[0] = {.level = AlertLevel::Advisory};
        sky.alerts.count    = 1;
        WeatherLights both;
        both.precip.set(slot(kWallC));
        both.alerts.set(slot(kWallC));

        const ProgramConfig on;
        const auto          pulse = core::weatherPulse(kWallC, true, Mode::Daylight, on, sky, both);
        expect(pulse && pulse->shade.hue_deg == 50); // advisory yellow, not snow's blue

        // The declared lights: WallC takes precipitation only, WallA nothing.
        const auto snow = core::weatherPulse(kWallC, true, Mode::Daylight, on, sky);
        expect(snow && snow->shade.hue_deg == 195);
        expect(!core::weatherPulse(kWallA, true, Mode::Daylight, on, sky));

        ProgramConfig off;
        off.weather_enabled = false;
        expect(!core::weatherPulse(kWallC, true, Mode::Daylight, off, sky, both));
    };
};
