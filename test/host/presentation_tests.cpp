// Shared status display data: the platform-neutral presentation consumed by
// the LCD and the future browser renderer.

import std.compat;
import sunsim.ev;
import sunsim.presentation;
import sunsim.test;

using namespace sunsim::ev;
using namespace sunsim::presentation;
using namespace sunsim::testing;
using namespace std::chrono_literals;

const suite<"presentation"> presentationTests = [] {
    "time helpers preserve LCD formatting"_test = [] {
        expect(clockText(24h) == "24:00");
        expect(clockText(25h) == "01:00");
        expect(durationText(2h + 30min) == "2h 30m");
    };

    "blank status has the LCD waiting text and layout"_test = [] {
        const StatusDisplayData data = makeStatusDisplayData(StatusSnapshot{});

        expect(data.time == "--:--");
        expect(data.day_length == "Waiting for");
        expect(data.next_event == "time sync...");
        expect(!data.time_valid && data.sun_elevation_deg == -90.0);
        expect(data.sunrise == "Rise --:--" && data.sunset == "Set --:--");
        expect(!data.cloud_percent && data.precipitation == Precip::None);
        expect(data.mode == "idle" && data.mode_tone == Tone::Muted);
        expect(data.sky.empty() && data.sky_tone == Tone::Sky);
        expect(data.groups.size() == kLayout.size());
        expect(data.groups[0].name == "Wall" && data.groups[0].label == 'D' && data.groups[0].count == 5);
        expect(data.groups[1].name == "Work" && data.groups[1].label == 'W');
        expect(data.groups[2].name == "Desk" && data.groups[2].label == 'L' && data.groups[2].count == 1);
        expect(data.groups[3].name == "Grow" && data.groups[3].label == 'G' && data.groups[3].count == 1);
    };

    "sky presentation prefers precipitation to cloud cover"_test = [] {
        StatusSnapshot status{};
        status.sky.cloud_percent = 80;
        status.sky.conditions = Conditions{.cloud_percent = 80, .precip = Precip::Rain};
        const StatusDisplayData rainy = makeStatusDisplayData(status);
        expect(rainy.sky == "rain");
        expect(rainy.cloud_percent == 80 && rainy.precipitation == Precip::Rain);

        status.sky.conditions->precip = Precip::None;
        const StatusDisplayData cloudy = makeStatusDisplayData(status);
        expect(cloudy.sky == "cloud 80%");
        expect(cloudy.cloud_percent == 80 && cloudy.precipitation == Precip::None);
    };

    "live status converts time, zone, sky and link into display data"_test = [] {
        StatusSnapshot status{};
        status.time = at(12h);
        status.time.simulated = true;
        status.day = {.sunrise = 6h, .sunset = 18h, .day_length = 12h,
                      .sun_azimuth_deg = 210.0, .sun_elevation_deg = 35.0,
                      .next_is_sunrise = false, .next_event = 18h, .until_next = 2h + 30min};
        status.zones[kZoneIds[0]] = {.mode = Mode::RedAlert, .gate_open = true};
        status.sky.alerts.count = 1;
        status.sky.alerts.items[0].level = AlertLevel::Warning;
        status.devices[kWallA].link = DeviceLink::Online;

        const StatusDisplayData data = makeStatusDisplayData(status);

        expect(data.time == "12:00" && data.weekday == "sim Tue");
        expect(data.time_valid && data.sunrise == "Rise 06:00" && data.sunset == "Set 18:00");
        expect(data.sun_azimuth_deg == 210.0 && data.sun_elevation_deg == 35.0);
        expect(data.day_length == "Day 12h 00m");
        expect(data.next_event == "Set 2h 30m  18:00");
        expect(data.mode == "RED ALERT" && data.mode_tone == Tone::Warning);
        expect(data.sky == "warning" && data.sky_tone == Tone::Warning);
        expect(data.device_links[kWallA] == DeviceLink::Online);
    };
};
