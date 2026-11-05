//! The platform's InternetSync on a fake internet: which jobs run when, in
//! which order, over one session; what they post; and the saved Wi-Fi networks.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::{Arc, Mutex};

use ev::*;
use platform::internet::{alerts_url, conditions_url, coordinate, forecast_url, place_forecast};
use platform::{Internet, InternetSync, Syncer, WifiNetwork, WifiSaveResult, WifiStore};
use sunsim_tests::*;

// 2026-06-23 (a Tuesday), UTC.
const DAY: i64 = days_from_civil(2026, 6, 23) * 86_400;
const fn utc(hours: i64, minutes: i64) -> SysSeconds {
    SysSeconds(DAY + hours * 3600 + minutes * 60)
}
// The board's zone: five hours behind.
const OFFSET: Duration = Duration::hours(-5);

const CONDITIONS: &str = r#"{"current":{"cloud_cover":40,"weather_code":61}}"#;
const ALERTS: &str = r#"{"features":[{"properties":{"status":"Actual","messageType":"Alert","severity":"Severe",
    "event":"Severe Thunderstorm Warning","ends":"2026-06-23T20:00:00-05:00"}}]}"#;

// Open-Meteo's hours from yesterday 00:00 UTC on: 30% except 75% at 17:00 UTC today (12:00 local).
fn forecast() -> String {
    let start = DAY - 86_400;
    let times: Vec<String> = (0..96).map(|h| (start + h * 3600).to_string()).collect();
    let covers: Vec<&str> = (0..96).map(|h| if h == 24 + 17 { "75" } else { "30" }).collect();
    format!(r#"{{"hourly":{{"time":[{}],"cloud_cover":[{}]}}}}"#, times.join(","), covers.join(","))
}

/// What the fake internet did, and what it will do.
#[derive(Default)]
struct World {
    wall: SysSeconds,
    wifi: bool,        // a network is in reach
    time_server: bool, // SNTP answers
    sessions: u32,     // connects
    disconnects: u32,
    calls: Vec<String>, // "connect", "clock", the URLs fetched, "disconnect"
}

#[derive(Clone, Default)]
struct Net(Arc<Mutex<World>>);

impl Internet for Net {
    fn connect(&mut self) -> bool {
        let mut w = self.0.lock().unwrap();
        w.sessions += 1;
        w.calls.push("connect".into());
        w.wifi
    }
    fn disconnect(&mut self) {
        let mut w = self.0.lock().unwrap();
        w.disconnects += 1;
        w.calls.push("disconnect".into());
    }
    fn sync_clock(&mut self) -> Result<(), String> {
        let mut w = self.0.lock().unwrap();
        w.calls.push("clock".into());
        if !w.time_server {
            return Err("no time server answered".into());
        }
        if w.wall < utc(0, 0) {
            w.wall = utc(9, 0); // set: 04:00 local
        }
        Ok(())
    }
    fn get(&mut self, url: &str, headers: &[(&str, &str)], sink: &mut dyn FnMut(&[u8])) -> Result<(), String> {
        self.0.lock().unwrap().calls.push(url.to_string());
        let body = if url.contains("hourly=") {
            forecast()
        } else if url.contains("current=") {
            CONDITIONS.to_string()
        } else {
            assert!(headers.iter().any(|(h, _)| *h == "User-Agent"), "NWS asks for a User-Agent");
            ALERTS.to_string()
        };
        for chunk in body.as_bytes().chunks(50) {
            sink(chunk);
        }
        Ok(())
    }
    fn wall(&self) -> SysSeconds {
        self.0.lock().unwrap().wall
    }
    fn boot(&self) -> Instant {
        Instant::from_boot(secs(100))
    }
    fn utc_offset(&self, _: SysSeconds) -> Duration {
        OFFSET
    }
}

impl Net {
    fn up(wall: SysSeconds) -> Self {
        let net = Net::default();
        {
            let mut w = net.0.lock().unwrap();
            w.wall = wall;
            w.wifi = true;
            w.time_server = true;
        }
        net
    }
    fn at(&self, wall: SysSeconds) {
        self.0.lock().unwrap().wall = wall;
    }
    fn calls(&self) -> Vec<String> {
        std::mem::take(&mut self.0.lock().unwrap().calls)
    }
    // What a session fetched: the jobs, by name.
    fn jobs(&self) -> Vec<&'static str> {
        self.calls()
            .iter()
            .filter_map(|c| match c.as_str() {
                "clock" => Some("clock"),
                u if u.contains("hourly=") => Some("forecast"),
                u if u.contains("current=") => Some("conditions"),
                u if u.contains("alerts") => Some("alerts"),
                _ => None,
            })
            .collect()
    }
}

// A Syncer posting to a log.
fn syncer(net: &Net) -> (Syncer<Net>, Rc<RefCell<Vec<Event>>>, EventBus) {
    let mut bus = EventBus::new();
    let log = Rc::new(RefCell::new(Vec::new()));
    let recorder = log.clone();
    bus.on_any(move |e| recorder.borrow_mut().push(e.clone()));
    (Syncer::new(bus.post().clone(), net.clone(), 44.98, -93.265), log, bus)
}

fn posted<T: Payload>(log: &Rc<RefCell<Vec<Event>>>, bus: &mut EventBus) -> Vec<T> {
    bus.drain();
    log.borrow().iter().filter_map(T::of).cloned().collect()
}

#[test]
fn the_boot_sync_runs_every_job_in_order_in_one_session() {
    let net = Net::up(utc(14, 0)); // 09:00 local: outside the window, but at boot everything runs
    let (mut s, log, mut bus) = syncer(&net);
    s.run(true);
    let calls = net.calls();
    assert_eq!(calls.first().map(String::as_str), Some("connect"));
    assert_eq!(calls.last().map(String::as_str), Some("disconnect"));
    assert_eq!(calls.len(), 6);
    assert!(
        calls[1] == "clock"
            && calls[2].contains("hourly=")
            && calls[3].contains("current=")
            && calls[4].contains("alerts")
    );

    let forecast = posted::<CloudCoverUpdated>(&log, &mut bus)[0].forecast;
    assert_eq!(forecast.day, Some(LocalDays::ymd(2026, 6, 23))); // local today
    assert_eq!(forecast.percent[12], 75); // 17:00 UTC is local noon
    assert_eq!(forecast.percent[11], 30);
    assert_eq!(forecast.percent[47], 30); // tomorrow's last local hour is covered too
    let now = posted::<ConditionsUpdated>(&log, &mut bus)[0];
    assert!(now.now.cloud_percent == 40 && now.now.precip == Precip::Rain && now.at == Instant::from_boot(secs(100)));
    let alerts = posted::<AlertsUpdated>(&log, &mut bus)[0].alerts;
    assert_eq!(alerts.count, 1);
    assert_eq!(alerts.items[0].event(), "Severe Thunderstorm Warning");
    assert_eq!(alerts.items[0].until, Instant::from_boot(secs(100)) + hours(11)); // 20:00 local, from 09:00 local
}

#[test]
fn the_weather_now_every_15_minutes_the_clock_and_forecast_once_a_day_in_the_window() {
    let net = Net::up(utc(14, 0));
    let (mut s, _log, _bus) = syncer(&net);
    s.run(true);
    net.calls();

    net.at(utc(14, 10));
    s.run(false);
    assert!(net.calls().is_empty()); // nothing due: no session at all

    net.at(utc(14, 15));
    s.run(false);
    assert_eq!(net.jobs(), ["conditions", "alerts"]);

    // The window, 04:00-05:00 local, is 09:00-10:00 UTC: tomorrow's.
    net.at(SysSeconds(utc(9, 5).0 + 86_400));
    s.run(false);
    assert_eq!(net.jobs(), ["clock", "forecast", "conditions", "alerts"]);
    net.at(SysSeconds(utc(9, 30).0 + 86_400));
    s.run(false);
    assert_eq!(net.jobs(), ["conditions", "alerts"]); // done for today
}

#[test]
fn a_failed_daily_job_is_retried_every_10_minutes_within_the_window() {
    let net = Net::up(SysSeconds(utc(14, 0).0));
    let (mut s, _log, _bus) = syncer(&net);
    s.run(true);
    net.calls();

    net.0.lock().unwrap().time_server = false;
    net.at(SysSeconds(utc(9, 0).0 + 86_400)); // tomorrow's window opens
    s.run(false);
    assert_eq!(net.jobs(), ["clock", "forecast", "conditions", "alerts"]);
    net.at(SysSeconds(utc(9, 5).0 + 86_400));
    s.run(false);
    assert!(net.jobs().is_empty()); // too soon to retry
    net.0.lock().unwrap().time_server = true;
    net.at(SysSeconds(utc(9, 10).0 + 86_400));
    s.run(false);
    assert_eq!(net.jobs(), ["clock"]); // the retry; the forecast went through
    net.at(SysSeconds(utc(10, 20).0 + 86_400));
    s.run(false);
    assert_eq!(net.jobs(), ["conditions", "alerts"]); // the window has closed
}

#[test]
fn until_the_clock_is_set_only_its_sync_can_run_and_everything_is_retried_every_10_minutes() {
    let net = Net::up(SysSeconds(60)); // 1970: never set
    net.0.lock().unwrap().time_server = false;
    let (mut s, log, mut bus) = syncer(&net);
    s.run(true);
    assert_eq!(net.jobs(), ["clock"]); // HTTPS can't check certificates without it
    assert!(posted::<CloudCoverUpdated>(&log, &mut bus).is_empty());

    net.at(SysSeconds(60 + 5 * 60));
    s.run(false);
    assert!(net.jobs().is_empty());
    net.0.lock().unwrap().time_server = true;
    net.at(SysSeconds(60 + 10 * 60)); // out of any window, but the lights are off without a clock
    s.run(false);
    assert_eq!(net.jobs(), ["clock", "forecast", "conditions", "alerts"]);
}

#[test]
fn without_wi_fi_nothing_runs_and_the_session_still_ends() {
    let net = Net::up(utc(14, 0));
    net.0.lock().unwrap().wifi = false;
    let (mut s, log, mut bus) = syncer(&net);
    s.run(true);
    assert!(net.jobs().is_empty());
    let w = net.0.lock().unwrap();
    assert!(w.sessions == 1 && w.disconnects == 1);
    drop(w);
    assert!(posted::<ConditionsUpdated>(&log, &mut bus).is_empty());
}

#[test]
fn start_waits_for_the_boot_sync_and_a_sync_request_runs_everything_again() {
    let net = Net::up(utc(14, 0));
    let (s, _log, _bus) = syncer(&net);
    let mut sync = InternetSync::start(s, |task| {
        std::thread::spawn(task);
    });
    assert_eq!(net.0.lock().unwrap().disconnects, 1); // the boot sync is done
    net.calls();
    sync.on(&SyncRequested.into());
    sync.on(&SyncRequested.into()); // asking twice: one sync
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while net.0.lock().unwrap().disconnects < 2 {
        assert!(std::time::Instant::now() < deadline, "the requested sync ran");
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    std::thread::sleep(std::time::Duration::from_millis(50));
    assert_eq!(net.0.lock().unwrap().disconnects, 2);
}

#[test]
fn coordinates_and_urls_as_the_services_like_them() {
    assert_eq!(coordinate(44.98), "44.98");
    assert_eq!(coordinate(-93.26543), "-93.2654");
    assert_eq!(coordinate(12.0), "12");
    assert_eq!(
        forecast_url(44.98, -93.265),
        "https://api.open-meteo.com/v1/forecast?latitude=44.98&longitude=-93.265\
         &hourly=cloud_cover&timeformat=unixtime&timezone=GMT&past_days=1&forecast_days=3"
    );
    assert!(
        conditions_url(1.5, 2.0)
            .ends_with("latitude=1.5&longitude=2&current=cloud_cover,weather_code&timeformat=unixtime")
    );
    assert_eq!(alerts_url(44.98, -93.265), "https://api.weather.gov/alerts/active?status=actual&point=44.98,-93.265");
}

#[test]
fn the_forecast_is_placed_by_local_hour_and_needs_one_inside_the_48() {
    let today = LocalDays::ymd(2026, 6, 23);
    let east = |t: SysSeconds| LocalSeconds(t.0) + hours(2);
    let hour = |start, cloud_percent| weather::Hour { start, cloud_percent };
    let f = place_forecast(&[hour(utc(10, 0), 55), hour(utc(22, 0), 80)], today, east).unwrap();
    assert!(f.percent[12] == 55 && f.percent[24] == 80 && f.percent[0] == CloudCover::NO_DATA);
    assert!(place_forecast(&[hour(SysSeconds(DAY - 86_400), 10)], today, east).is_none()); // yesterday only
}

#[test]
fn wi_fi_networks_saved_in_slots_with_the_old_key_and_the_build_as_fallbacks() {
    let fallback = WifiNetwork { ssid: "build".into(), password: "pw".into() };
    let mut store = WifiStore::new(Nvs::default(), Some(fallback.clone()));
    let loaded = store.load();
    assert!(loaded.from_build && loaded.networks == [fallback]);

    assert_eq!(store.add("home", "secret"), WifiSaveResult::Added);
    assert_eq!(store.add("work", ""), WifiSaveResult::Added); // open
    assert_eq!(store.add("home", "changed"), WifiSaveResult::Updated);
    assert_eq!(store.add("", "x"), WifiSaveResult::Invalid);
    assert_eq!(store.add(&"x".repeat(32), ""), WifiSaveResult::Invalid);
    let loaded = store.load();
    assert!(!loaded.from_build);
    assert_eq!(
        loaded.networks.iter().map(|n| (n.ssid.as_str(), n.password.as_str())).collect::<Vec<_>>(),
        [("home", "changed"), ("work", "")]
    );
    for ssid in ["a", "b", "c"] {
        assert_eq!(store.add(ssid, ""), WifiSaveResult::Added);
    }
    assert_eq!(store.add("d", ""), WifiSaveResult::Full);
    assert!(store.remove("work") && !store.remove("work"));
    assert_eq!(store.load().networks.len(), 4);

    // As the C++ firmware kept one network, before the slots: listed first,
    // and moved into the slots on the next change.
    let old = Nvs::default()
        .with("wifi.ssid", Value::Str("old".into()))
        .with("wifi.pass", Value::Str("pw".into()))
        .with("wifi0.ssid", Value::Str("new".into()));
    let mut store = WifiStore::new(old, None);
    assert_eq!(store.load().networks.iter().map(|n| n.ssid.as_str()).collect::<Vec<_>>(), ["old", "new"]);
    assert_eq!(store.add("third", ""), WifiSaveResult::Added);
    assert_eq!(store.load().networks.iter().map(|n| n.ssid.as_str()).collect::<Vec<_>>(), ["old", "new", "third"]);

    store.clear();
    assert!(store.load().networks.is_empty());
}
