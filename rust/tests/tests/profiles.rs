//! Runtime installation profiles: the JSON, the builder's checks, the core
//! running a profile's zones, the store that keeps one in NVS, and the
//! console upload that fills it.

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;

use ev::*;
use platform::presentation::program_flow;
use platform::{Console, ConsoleBoard, ProfileStore, StatusSlot, WifiStore};
use sunsim_tests::*;

const STUDIO: &str = include_str!("../../../web/public/profiles/studio.json");
const OFFICE: &str = include_str!("../../../web/public/profiles/office.json");

#[test]
fn the_studio_profile_regroups_the_lights() {
    let studio = parse_profile(STUDIO).unwrap();
    let names: Vec<_> = studio.zones.iter().map(|z| z.name.as_str()).collect();
    assert_eq!(names, ["studio", "plants"]);
    let t = &studio.topology;
    assert_eq!(t.zone_of[DESK], Some(ZoneId(0)));
    assert_eq!(t.zone_of[GROW], Some(ZoneId(1)));
    assert!(t.controls_of[BUTTON].is_some());
    assert!(t.weather.precip.contains(WALL_C) && t.weather.alerts.contains(WALL_D));
    assert!(matches!(t.look_of[DESK], Look::OnOffOnly));
    assert!(matches!(t.look_of[WORK], Look::WorkCurve(_)));
}

#[test]
fn the_office_profile_is_the_built_in_installation() {
    let office = parse_profile(OFFICE).unwrap();
    let built_in = Installation::built_in();
    assert_eq!(office.zones.len(), built_in.zones.len());
    for role in ROLES {
        assert_eq!(office.topology.zone_of[role], built_in.topology.zone_of[role], "{role:?}");
        assert_eq!(office.topology.peak_of[role], built_in.topology.peak_of[role], "{role:?}");
    }
}

// The studio, with a change made to its JSON.
fn studio_with(from: &str, to: &str) -> Result<Installation, String> {
    assert!(STUDIO.contains(from), "{from}");
    parse_profile(&STUDIO.replacen(from, to, 1))
}

#[test]
fn a_profile_that_doesnt_fit_the_hardware_is_refused() {
    assert!(parse_profile("{").is_err());
    assert!(parse_profile(r#"{"schemaVersion": 2, "zones": []}"#).is_err());
    // Every light in exactly one zone.
    assert!(studio_with(r#"{ "role": "Desk", "look": "on-off" }"#, r#"{ "role": "Grow", "look": "on-off" }"#).is_err());
    assert!(
        studio_with(
            r#",
        { "role": "Desk", "look": "on-off" }"#,
            ""
        )
        .is_err()
    );
    // A plug only switches; a bulb isn't a plug.
    assert!(studio_with(r#""role": "Desk", "look": "on-off""#, r#""role": "Desk", "look": "sun-curve""#).is_err());
    assert!(studio_with(r#""role": "WallA", "look": "sun-curve""#, r#""role": "WallA", "look": "on-off""#).is_err());
    // A window inside the day; a peak of 1-100.
    assert!(studio_with(r#""endMinute": 1380"#, r#""endMinute": 1441"#).is_err());
    assert!(studio_with(r#""startMinute": 420"#, r#""startMinute": 1380"#).is_err());
    assert!(studio_with(r#""look": "work-curve""#, r#""look": "work-curve", "peakPercent": 0"#).is_err());
    // Weather on the zone's own color lights.
    assert!(studio_with(r#""precipitation": ["WallC"]"#, r#""precipitation": ["Grow"]"#).is_err());
    // Names unique, and short.
    assert!(studio_with(r#""name": "plants""#, r#""name": "studio""#).is_err());
    assert!(studio_with(r#""name": "plants""#, &format!(r#""name": "{}""#, "p".repeat(32))).is_err());
    assert!(studio_with(r#""look": "work-curve""#, r#""look": "work-curve", "peakPercent": 40"#).is_ok());
}

#[test]
fn the_builder_checks_each_step() {
    let mut b = ProfileBuilder::new();
    assert!(!b.begin(0) && !b.begin(9));
    assert!(b.begin(1));
    assert!(b.set_zone(0, "all", 0, 1440));
    assert!(!b.set_zone(1, "beyond", 0, 1440));
    for role in 0..5 {
        assert!(b.add_member(0, role, 0, 100));
    }
    assert!(!b.add_member(0, 0, 0, 100)); // WallA's already in
    assert!(!b.add_member(0, 8, 0, 100)); // the button isn't a light
    assert!(b.add_member(0, 5, 1, 50));
    assert!(!b.finish()); // the plugs aren't in yet
    assert!(b.add_member(0, 6, 2, 100) && b.add_member(0, 7, 2, 100));
    assert!(b.set_button(0, 8) && b.add_weather_role(0, 2, 0));
    assert!(!b.add_weather_role(0, 6, 1)); // a plug can't pulse
    assert!(b.finish());
    let all = b.into_installation().unwrap();
    assert_eq!(all.zones.len(), 1);
    assert_eq!(all.topology.peak_of[WORK], 50);
}

#[test]
fn the_core_runs_a_profiles_zones() {
    let mut h = Harness::with_installation(PerRole::filled(0), parse_profile(STUDIO).unwrap());
    let slot = Rc::new(std::cell::RefCell::new(StatusSlot::default()));
    h.bus.attach(&slot);
    h.tick(2);
    let status = slot.borrow().get().unwrap();
    let flow = program_flow(&status, &parse_profile(STUDIO).unwrap());
    let zones: Vec<_> = flow.iter().map(|z| z.name.as_str()).collect();
    assert_eq!(zones, ["studio", "plants"]);
    assert!(flow[0].gate_open && !flow[1].gate_open); // noon: the studio's hours, not the plants'
    assert!(flow[0].members.iter().any(|m| m.role == DESK && m.behavior == "on/off"));
    assert_eq!(flow[1].members.len(), 1);
    // The button drives the studio, which has the Desk in it now.
    assert!(flow[0].available_modes.iter().any(|c| c.mode == Mode::RedAlert && c.button_press == Some(Press::Double)));
    assert!(flow[1].available_modes.iter().all(|c| c.button_press.is_none()));
}

#[test]
fn the_built_in_flow_shows_each_zones_gate_modes_and_members() {
    let mut h = Harness::new();
    let slot = Rc::new(std::cell::RefCell::new(StatusSlot::default()));
    h.bus.attach(&slot);
    h.tick(2);
    let flow = program_flow(&slot.borrow().get().unwrap(), &Installation::built_in());
    let zones: Vec<_> = flow.iter().map(|z| z.name.as_str()).collect();
    assert_eq!(zones, ["office", "desk-downlight", "bookcase-grow-light"]);
    assert_eq!(flow[0].mode, Mode::Daylight);
    assert!(flow[0].gate_open && flow[0].gate_applies);
    assert_eq!(flow[0].members.len(), 6);
    assert!(flow[0].members.iter().all(|m| m.behavior != "on/off"));
}

// ---- kept in NVS ------------------------------------------------------------

#[test]
fn the_store_saves_only_a_whole_valid_profile() {
    let mut store = ProfileStore::new(Nvs::default());
    assert!(store.load().is_none());
    let bytes = STUDIO.as_bytes();
    assert!(store.begin_upload(bytes.len()));
    assert!(!store.append(1, &bytes[..10])); // out of order
    for (i, chunk) in bytes.chunks(24).enumerate() {
        assert!(store.append(i * 24, chunk));
    }
    store.commit().unwrap();
    assert_eq!(store.load().unwrap().zones[1].name, "plants");

    // A broken one is refused, and the saved one stays.
    let broken = b"{\"schemaVersion\": 1}";
    assert!(store.begin_upload(broken.len()) && store.append(0, broken));
    assert!(store.commit().is_err());
    assert_eq!(store.load().unwrap().zones.len(), 2);
    // Short of its size: refused too.
    assert!(store.begin_upload(10) && store.append(0, b"{}"));
    assert!(store.commit().is_err());

    assert!(!store.begin_upload(0) && !store.begin_upload(3073));
    assert!(store.clear());
    assert!(store.load().is_none());
}

#[test]
fn an_invalid_saved_profile_is_ignored() {
    let nvs = Nvs::default().with("install", Value::Str("{\"zones\": 7}".into()));
    assert!(ProfileStore::new(nvs).load().is_none());
}

// ---- uploaded over the console ----------------------------------------------

struct Board {
    restarted: Rc<Cell<bool>>,
}

impl ConsoleBoard for Board {
    fn now(&self) -> Instant {
        Instant::ZERO
    }
    fn restart(&mut self) {
        self.restarted.set(true);
    }
}

#[test]
fn the_console_uploads_a_profile_in_chunks_and_restarts_to_apply_it() {
    let mut h = Harness::new();
    let slot = Rc::new(std::cell::RefCell::new(StatusSlot::default()));
    h.bus.attach(&slot);
    let restarted = Rc::new(Cell::new(false));
    let shared = slot.borrow().clone();
    let mut console = Console::new(
        h.bus.post().clone(),
        shared,
        Arc::new(Installation::built_in()),
        WifiStore::new(Nvs::default(), None),
        ProfileStore::new(Nvs::default()),
        Board { restarted: restarted.clone() },
        120,
    );
    fn type_in(console: &mut Console<Nvs, Board>, line: &str) -> String {
        let reply = console.line(line);
        console.finish_reply();
        reply
    }
    macro_rules! ty {
        ($line:expr) => {
            type_in(&mut console, &$line)
        };
    }
    let bytes = STUDIO.as_bytes();
    assert_eq!(
        ty!(format!("profile begin {}", bytes.len())),
        format!("profile upload started ({} bytes)\n", bytes.len())
    );
    assert!(ty!("profile chunk 0 zz").starts_with("profile chunk needs"));
    assert!(ty!(format!("profile chunk 0 {}", "41".repeat(25))).starts_with("profile chunk needs")); // 25 bytes
    for (i, chunk) in bytes.chunks(24).enumerate() {
        let hex: String = chunk.iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(ty!(format!("profile chunk {} {hex}", i * 24)), "profile chunk accepted\n");
    }
    assert!(!restarted.get());
    assert_eq!(ty!("profile commit"), "profile saved; restarting to apply\n");
    assert!(restarted.get());
    assert_eq!(console.profiles().load().unwrap().zones[0].name, "studio");

    restarted.set(false);
    assert!(ty!("profile commit").starts_with("profile rejected (no upload)"));
    assert!(!restarted.get());
    assert!(ty!("profile clear").starts_with("saved profile cleared"));
    assert!(restarted.get() && console.profiles().load().is_none());
    assert!(ty!("profile").starts_with("usage: profile"));
}
