//! The weather: reading the services' replies, SkyModel, and the weather lights.

use ev::*;
use sunsim_core::weather_pulse;
use sunsim_tests::*;
use weather::{
    AlertsReader, ConditionsReader, ForecastReader, JsonScanner, JsonSink, Scalar, alert_level_of, parse_iso_time,
    precip_of,
};

// The program's base targets, then the weather's: the harness ticks on.
fn weather_rig(h: &mut Harness, time: TimeReading) {
    h.tick_at(1, time);
    h.post(ProgramTick { time, transition: Transition::secs(60) });
    h.clear_log();
}

fn outputs(h: &Harness, role: Role) -> Vec<OutputTarget> {
    h.all::<OutputTarget>().into_iter().filter(|t| t.role == role).collect()
}

fn snowing(at: Instant, intensity: Intensity) -> ConditionsUpdated {
    ConditionsUpdated { now: Conditions { cloud_percent: 100, precip: Precip::Snow, intensity }, at }
}

fn alerted(at: Instant, level: AlertLevel) -> AlertsUpdated {
    let mut a = AlertsUpdated { at, ..AlertsUpdated::default() };
    let mut alert = Alert { level, until: at + hours(2), ..Alert::default() };
    alert.set_event("X");
    a.alerts.push(alert);
    a
}

fn hue_of(t: &OutputTarget) -> Option<HueSat> {
    t.state.color.hue_sat()
}

#[derive(Default)]
struct Seen {
    scalars: Vec<String>,
    closed: Vec<String>,
}

impl JsonSink for Seen {
    fn scalar(&mut self, s: &Scalar) {
        self.scalars.push(format!("{}/{}={}", s.parent, s.key, s.text));
    }
    fn object_end(&mut self, key: &str) {
        self.closed.push(key.to_string());
    }
}

#[test]
fn the_json_scanner_keys_parents_and_escapes_whatever_the_chunk_boundaries() {
    let bs = "\\"; // spelled out: the escape stays for the scanner to read
    let json =
        format!(r#"{{"a": {{"b": 1.5, "c": [10, null, "x\"y"]}}, "d": [{{"e": true}}], "f": "{bs}u00e9t{bs}u00e9"}}"#);
    let json = json.as_bytes();
    for chunk in [1, 3, json.len()] {
        let mut scanner = JsonScanner::new();
        let mut seen = Seen::default();
        for piece in json.chunks(chunk) {
            scanner.feed(piece, &mut seen);
        }
        assert_eq!(seen.scalars, ["a/b=1.5", "a/c=10", "a/c=null", "a/c=x\"y", "d/e=true", "/f=?t?"]);
        assert_eq!(seen.closed, ["a", "d", ""]);
    }
}

#[test]
fn open_meteos_current_conditions_cloud_and_whats_falling_units_skipped() {
    let reply = concat!(
        r#"{"latitude":44.98,"longitude":-93.27,"current_units":{"time":"unixtime","interval":"seconds","#,
        r#""cloud_cover":"%","weather_code":"wmo code"},"current":{"time":1760629500,"interval":900,"#,
        r#""cloud_cover":87,"weather_code":73}}"#
    );
    let mut reader = ConditionsReader::new();
    for piece in reply.as_bytes().chunks(5) {
        reader.feed(piece);
    }
    let now = reader.conditions().unwrap();
    assert!(now.cloud_percent == 87 && now.precip == Precip::Snow && now.intensity == Intensity::Moderate);

    let mut empty = ConditionsReader::new();
    empty.feed(br#"{"error":true,"reason":"bad"}"#);
    assert!(empty.conditions().is_none());

    assert_eq!(precip_of(3).0, Precip::None); // overcast
    assert_eq!(precip_of(45).0, Precip::None); // fog
    assert_eq!(precip_of(82), (Precip::Rain, Intensity::Heavy));
    assert_eq!(precip_of(67).0, Precip::FreezingRain);
    assert_eq!(precip_of(99).0, Precip::Thunderstorm);
}

#[test]
fn open_meteos_hourly_forecast_times_paired_with_cover_nulls_dropped() {
    let mut reader = ForecastReader::new();
    reader.feed(
        concat!(
            r#"{"hourly_units":{"time":"unixtime","cloud_cover":"%"},"#,
            r#""hourly":{"time":[1760572800,1760576400,1760580000],"cloud_cover":[12,null,100]}}"#
        )
        .as_bytes(),
    );
    let hours = reader.hours();
    assert_eq!(hours.len(), 2);
    assert!(hours[0].start == SysSeconds(1760572800) && hours[0].cloud_percent == 12);
    assert!(hours[1].start == SysSeconds(1760580000) && hours[1].cloud_percent == 100);
}

#[test]
fn nws_times_offsets_and_z() {
    let t = parse_iso_time("2026-10-16T18:00:00-05:00");
    let z = parse_iso_time("2026-10-16T23:00:00Z");
    assert!(t.is_some() && t == z);
    assert_eq!(parse_iso_time("2026-10-17T01:30:00+02:30"), parse_iso_time("2026-10-16T23:00:00Z"));
    assert!(parse_iso_time("soon").is_none());
    assert!(parse_iso_time("2026-13-01T00:00:00Z").is_none());
}

#[test]
fn nws_alerts_tiers_by_name_one_per_event_cancellations_and_statements_dropped() {
    // As api.weather.gov sends it: pretty-printed GeoJSON, nested
    // references and parameters, long texts, a null `ends`.
    let reply = r#"{
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
}"#;
    let mut reader = AlertsReader::new();
    for piece in reply.as_bytes().chunks(7) {
        reader.feed(piece);
    }
    let all = reader.alerts();
    assert_eq!(all.len(), 3); // the watch twice, the warning
    assert!(all[0].event == "Winter Storm Watch" && all[0].level == AlertLevel::Watch);
    assert_eq!(all[2].level, AlertLevel::Warning);

    let wall_now = parse_iso_time("2026-10-16T11:00:00-05:00").unwrap();
    let boot_now = Instant::from_boot(secs(100));
    let in_force = reader.in_force(wall_now, boot_now);
    assert_eq!(in_force.count, 2);
    assert_eq!(in_force.items[0].event(), "Tornado Warning");
    assert_eq!(in_force.items[0].until, boot_now + mins(45));
    assert_eq!(in_force.items[1].event(), "Winter Storm Watch");
    assert_eq!(in_force.items[1].until, boot_now + hours(19)); // the update's end, not the original's

    assert_eq!(reader.in_force(wall_now + hours(1), boot_now).count, 1); // the warning has ended

    assert_eq!(alert_level_of("Tornado Emergency", "Extreme"), AlertLevel::Warning);
    assert_eq!(alert_level_of("Heat Advisory", "Moderate"), AlertLevel::Advisory);
    assert_eq!(alert_level_of("Hazardous Weather Outlook", "Minor"), AlertLevel::None);
}

#[test]
fn the_sky_live_cloud_cover_while_fresh_then_the_forecast_alerts_until_they_end() {
    let mut h = Harness::new();
    let sky = |h: &Harness| h.all::<SkyChanged>().last().unwrap().sky;
    h.tick(1);
    h.post(CloudCoverUpdated { forecast: tuesday_forecast() });
    assert!(sky(&h).cloud_percent == Some(20) && !sky(&h).cloud_live);

    let fetched = h.now;
    h.post(ConditionsUpdated {
        now: Conditions { cloud_percent: 90, precip: Precip::Rain, ..Conditions::default() },
        at: fetched,
    });
    assert!(
        sky(&h).cloud_percent == Some(90) && sky(&h).cloud_live && sky(&h).conditions.unwrap().precip == Precip::Rain
    );

    let mut alerts = AlertsUpdated { at: fetched, ..AlertsUpdated::default() };
    alerts.alerts.push(Alert { level: AlertLevel::Watch, until: fetched + mins(30), ..Alert::default() });
    h.post(alerts);
    assert_eq!(sky(&h).alert(), AlertLevel::Watch);

    let sim = TimeReading { simulated: true, ..noon() }; // fast-day plays the forecast's day
    h.tick_at(1, sim);
    assert!(sky(&h).cloud_percent == Some(20) && !sky(&h).cloud_live && sky(&h).conditions.is_some());
    h.tick(1);

    h.clear_log();
    h.now = fetched + mins(30) - secs(1);
    h.tick(1); // the watch has ended
    assert_eq!(h.all::<SkyChanged>().len(), 1);
    assert!(sky(&h).alert() == AlertLevel::None && sky(&h).cloud_live);

    h.now = fetched + WEATHER_FRESH_FOR - secs(1);
    h.tick(1); // the live reading is stale
    assert!(sky(&h).cloud_percent == Some(20) && !sky(&h).cloud_live && sky(&h).conditions.is_none());

    h.clear_log();
    h.tick(5);
    assert!(h.all::<SkyChanged>().is_empty()); // only when it changes
}

#[test]
fn snow_pulses_the_precipitation_light_which_goes_back_to_the_sun_after() {
    let mut h = Harness::new();
    weather_rig(&mut h, noon());
    h.post(snowing(h.now, Intensity::Moderate));
    h.tick(6);

    let c = outputs(&h, WALL_C);
    assert!(c.len() >= 3);
    assert!(c.iter().all(|t| t.state.on && hue_of(t).is_some_and(|hs| hs.saturation == 150)));
    assert!(c.iter().any(|t| t.state.level != c[0].state.level));
    assert!(outputs(&h, WALL_A).is_empty()); // the rest of the wall follows the sun alone

    // A program update for WallC meanwhile is held back.
    h.clear_log();
    h.post(ProgramTick { time: noon(), transition: Transition::secs(60) });
    assert!(outputs(&h, WALL_C).is_empty() && !outputs(&h, WALL_A).is_empty());

    h.clear_log();
    h.post(ConditionsUpdated { now: Conditions { cloud_percent: 100, ..Conditions::default() }, at: h.now }); // let go at once
    let back = outputs(&h, WALL_C);
    assert!(back.len() == 1 && hue_of(&back[0]).is_none() && back[0].level_transition == Transition::secs(2));
}

#[test]
fn snow_outside_active_hours_glows_dimmer_not_at_all_in_off_mode() {
    let night = at(hours(22));

    let mut lit = Harness::new();
    weather_rig(&mut lit, noon());
    lit.post(snowing(lit.now, Intensity::Heavy));
    lit.tick(6);
    let mut dark = Harness::new();
    weather_rig(&mut dark, night);
    dark.post(snowing(dark.now, Intensity::Heavy));
    dark.tick_at(6, night);

    let top = |h: &Harness| outputs(h, WALL_C).iter().map(|t| t.state.level).max().unwrap_or(0);
    assert_eq!(top(&lit), MAX_LEVEL);
    assert!(top(&dark) < MAX_LEVEL / 2 && top(&dark) > 0);

    dark.post(ButtonPressed { press: Press::Hold, at: dark.now, ..ButtonPressed::default() }); // all off
    dark.clear_log();
    dark.tick_at(6, night);
    assert!(outputs(&dark, WALL_C).iter().all(|t| !t.state.on));
}

#[test]
fn a_warning_pulses_red_even_with_the_lights_off_a_watch_only_on_lit_lights() {
    let night = at(hours(22));
    let mut h = Harness::new();
    weather_rig(&mut h, night);
    h.post(ButtonPressed { press: Press::Hold, at: h.now, ..ButtonPressed::default() }); // all off
    h.post(alerted(h.now, AlertLevel::Watch));
    h.clear_log();
    h.tick_at(4, night);
    assert!(outputs(&h, WALL_B).is_empty());

    h.post(alerted(h.now, AlertLevel::Warning));
    h.tick_at(4, night);
    for role in [WALL_B, WALL_D] {
        let alarm = outputs(&h, role);
        assert_eq!(alarm.len(), 5); // at once, then a fade every second
        assert!(alarm.iter().all(|t| t.state.on && hue_of(t).is_some_and(|hs| hs.hue == 0)));
    }
    assert!(outputs(&h, WALL_C).is_empty());

    h.clear_log();
    h.tick_at(1, night);
    h.now += WEATHER_FRESH_FOR; // no news since: stale
    h.tick_at(1, night);
    let released = outputs(&h, WALL_B);
    assert!(released.last().is_some_and(|t| !t.state.on));
}

#[test]
fn an_alert_outranks_the_precipitation_on_a_light_thats_both_weather_off_stops_both() {
    let mut sky = Sky {
        conditions: Some(Conditions { cloud_percent: 100, precip: Precip::Snow, ..Conditions::default() }),
        ..Sky::default()
    };
    sky.alerts.push(Alert { level: AlertLevel::Advisory, ..Alert::default() });
    let both = WeatherLights { precip: RoleSet::EMPTY.with(WALL_C), alerts: RoleSet::EMPTY.with(WALL_C) };

    let on = ProgramConfig::default();
    let pulse = weather_pulse(WALL_C, true, Mode::Daylight, &on, &sky, &both).unwrap();
    assert_eq!(pulse.shade.hue_deg, 50); // advisory yellow, not snow's blue

    // The declared lights: WallC takes precipitation only, WallA nothing.
    let snow = weather_pulse(WALL_C, true, Mode::Daylight, &on, &sky, &TOPOLOGY.weather).unwrap();
    assert_eq!(snow.shade.hue_deg, 195);
    assert!(weather_pulse(WALL_A, true, Mode::Daylight, &on, &sky, &TOPOLOGY.weather).is_none());

    let off = ProgramConfig { weather_enabled: false, ..on };
    assert!(weather_pulse(WALL_C, true, Mode::Daylight, &off, &sky, &both).is_none());
}
