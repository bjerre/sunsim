//! The browser simulator's office: what the page sees, as its JSON, through
//! the same steps as the C++ build's wasm smoke test (test/build2/wasm_smoke.mjs).

use serde_json::Value;
use web::Office;

fn state(o: &Office) -> Value {
    serde_json::from_str(&o.state_json()).expect("the state is JSON")
}

fn light<'a>(s: &'a Value, role: &str) -> &'a Value {
    s["lights"].as_array().unwrap().iter().find(|l| l["role"] == role).expect("a light")
}

fn on(s: &Value, role: &str) -> bool {
    light(s, role)["on"].as_bool().unwrap()
}

#[test]
fn the_office_as_the_page_sees_it_through_a_day_and_its_controls() {
    let mut o = Office::new();
    let s = state(&o);
    let roles: Vec<&str> = s["lights"].as_array().unwrap().iter().map(|l| l["role"].as_str().unwrap()).collect();
    assert_eq!(roles, ["WallA", "WallB", "WallC", "WallD", "WallE", "Work", "Desk", "Grow"]);
    assert_eq!(s["devices"].as_array().unwrap().len(), 9);
    assert!(s["devices"].as_array().unwrap().iter().all(|d| d["link"] == "online" || d["role"] == "Button"));
    assert_eq!(s["weekday"], "sim Tue");
    assert_eq!(s["mode"], "daylight");
    assert!(s["lights"].as_array().unwrap().iter().any(|l| l["on"] == true));
    assert!(on(&s, "Desk") && !on(&s, "Grow"));

    assert!(o.set_clock(2025, 1, 15, 5, 0, -420));
    let s = state(&o);
    assert_eq!(
        (s["time"].as_str(), s["utcOffsetMinutes"].as_i64(), s["simDate"].as_str()),
        (Some("05:00"), Some(-420), Some("2025-01-15"))
    );
    assert!(!on(&s, "Desk") && on(&s, "Grow"));
    assert!(s["sun"]["azimuthDeg"].is_f64() && s["sun"]["elevationDeg"].is_f64());
    assert!(!o.set_clock(2025, 2, 30, 12, 0, -420)); // no such day

    assert!(o.set_location(0.0, 0.0));
    assert!(o.set_clock(2026, 10, 25, 5, 0, 0));
    let s = state(&o);
    assert_eq!(s["time"], "05:00");
    assert!(s["nextEvent"].as_str().unwrap().starts_with("Rise ") && s["nextEvent"].as_str().unwrap().contains(" 05:"));

    assert!(o.set_location(47.6, -122.3));
    let s = state(&o);
    assert_eq!((s["location"]["latitude"].as_f64(), s["location"]["longitude"].as_f64()), (Some(47.6), Some(-122.3)));
    assert!(s["sun"]["elevationDeg"].as_f64().unwrap() < 0.0); // 05:00 UTC is Seattle's night
    assert!(o.set_clock(2025, 1, 15, 12, 0, -480));
    assert!(state(&o)["sun"]["elevationDeg"].as_f64().unwrap() > 0.0);
    assert!(o.set_clock(2025, 1, 15, 12, 0, -420));
    let s = state(&o);
    assert!(s["time"] == "12:00" && on(&s, "Work")); // a Wednesday
    assert!(o.set_clock(2025, 1, 18, 12, 0, -420));
    let s = state(&o);
    assert!(!on(&s, "Work") && on(&s, "Desk")); // Saturday: the office's days aren't the desk's
    assert!(o.set_clock(2025, 1, 15, 10, 0, -480));
    assert!(!o.set_location(91.0, -122.3) && !o.set_location(f64::NAN, 0.0));

    o.press(2); // hold: the office off
    let s = state(&o);
    assert_eq!(s["mode"], "off");
    assert!(["WallA", "WallB", "WallC", "WallD", "WallE", "Work"].iter().all(|r| !on(&s, r)));
    assert!(on(&s, "Desk")); // the plugs keep their schedules

    o.press(0);
    o.set_minute(600);
    let s = state(&o);
    assert_eq!(s["simMinute"], 600);
    assert!(s["lights"].as_array().unwrap().iter().any(|l| l["on"] == true));

    o.weather(2, 90);
    o.alert(3);
    let s = state(&o);
    assert!(s["sky"] == "warning" && s["skyTone"] == "warning");
}

#[test]
fn red_alert_beats_in_virtual_time_as_the_page_advances_it() {
    let mut o = Office::new();
    o.press(1);
    let level = |o: &Office| light(&state(o), "WallA")["level"].as_u64().unwrap();
    let first = level(&o);
    assert_eq!(light(&state(&o), "WallA")["color"]["mode"], "hue");
    o.advance(1);
    let second = level(&o);
    assert_ne!(first, second, "bright and dim alternate a second apart");
    assert_eq!(state(&o)["modeTone"], "warning");
}
