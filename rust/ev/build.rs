//! The room from config/installation.json, as Rust: LAYOUT (each group's
//! count, for types.rs) and ZONES (for installation.rs), which and topology_of checks
//! as they compile. This checks the document's shape (as
//! tools/generate_installation.py does for the C++ build); a mistake in it
//! stops the build with where it is.

use std::collections::HashSet;
use std::fmt::Write as _;
use std::path::Path;

use serde_json::{Map, Value};

const GROUPS: [(&str, &str); 5] =
    [("Wall", "ColorBulb"), ("Work", "ColorBulb"), ("Desk", "Plug"), ("Grow", "Plug"), ("Button", "Button")];
const KINDS: [(&str, &str); 3] = [("color-bulb", "ColorBulb"), ("plug", "Plug"), ("button", "Button")];
const MODES: [(&str, &str); 4] =
    [("daylight", "Daylight"), ("off", "Off"), ("red-alert", "RedAlert"), ("tuning", "Tuning")];

type Result<T> = std::result::Result<T, String>;

fn main() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../config/installation.json");
    println!("cargo:rerun-if-changed={}", path.display());
    let out = std::env::var("OUT_DIR").unwrap();
    match std::fs::read_to_string(&path)
        .map_err(|e| e.to_string())
        .and_then(|text| serde_json::from_str::<Value>(&text).map_err(|e| e.to_string()))
        .and_then(|document| generate(&document))
    {
        Ok((layout, zones)) => {
            std::fs::write(Path::new(&out).join("layout.rs"), layout).unwrap();
            std::fs::write(Path::new(&out).join("zones.rs"), zones).unwrap();
        }
        Err(why) => panic!("config/installation.json: {why}"),
    }
}

fn object<'a>(v: &'a Value, at: &str) -> Result<&'a Map<String, Value>> {
    v.as_object().ok_or(format!("{at} must be an object"))
}

fn array<'a>(v: &'a Value, at: &str) -> Result<&'a Vec<Value>> {
    v.as_array().ok_or(format!("{at} must be an array"))
}

fn string<'a>(v: &'a Value, at: &str) -> Result<&'a str> {
    v.as_str().ok_or(format!("{at} must be a string"))
}

fn integer(v: &Value, at: &str, low: i64, high: i64) -> Result<i64> {
    v.as_i64().filter(|n| (low..=high).contains(n)).ok_or(format!("{at} must be an integer from {low} to {high}"))
}

fn keys(o: &Map<String, Value>, required: &[&str], optional: &[&str], at: &str) -> Result<()> {
    if let Some(missing) = required.iter().find(|k| !o.contains_key(**k)) {
        return Err(format!("{at} is missing {missing}"));
    }
    if let Some(unknown) = o.keys().find(|k| !required.contains(&k.as_str()) && !optional.contains(&k.as_str())) {
        return Err(format!("{at} has an unknown field {unknown}"));
    }
    Ok(())
}

// "HH:MM" (or 24:00) as minutes.
fn minutes(v: &Value, at: &str) -> Result<i64> {
    let text = string(v, at)?;
    let bad = || format!("{at} must be a time in HH:MM format");
    if text == "24:00" {
        return Ok(24 * 60);
    }
    let (h, m) = text.split_once(':').filter(|_| text.len() == 5).ok_or_else(bad)?;
    let (h, m): (i64, i64) = (h.parse().map_err(|_| bad())?, m.parse().map_err(|_| bad())?);
    if !(0..24).contains(&h) || !(0..60).contains(&m) {
        return Err(format!("{at} is outside the 24-hour day"));
    }
    Ok(h * 60 + m)
}

// A {"setting": name} among `settings`, or the value `literal` makes of {"value": v}.
fn tunable(
    v: &Value,
    at: &str,
    settings: &[&str],
    literal: impl FnOnce(&Value, &str) -> Result<String>,
) -> Result<String> {
    let o = object(v, at)?;
    keys(o, &[], &["setting", "value"], at)?;
    match (o.get("setting"), o.get("value")) {
        (Some(name), None) => {
            let name = string(name, &format!("{at}.setting"))?;
            if !settings.contains(&name) {
                return Err(format!("{at} names an unsupported setting {name:?}"));
            }
            Ok(format!("setting!({name})"))
        }
        (None, Some(value)) => Ok(format!("Tunable::Fixed({})", literal(value, &format!("{at}.value"))?)),
        _ => Err(format!("{at} needs exactly one of setting or value")),
    }
}

// A schedule: {"setting": "active"|"work"} or {"start": "HH:MM", "end": "HH:MM"}.
fn hours(v: &Value, at: &str) -> Result<String> {
    let o = object(v, at)?;
    if o.contains_key("setting") {
        return tunable(v, at, &["active", "work"], |_, _| unreachable!());
    }
    keys(o, &["start", "end"], &[], at)?;
    let (start, end) = (minutes(&o["start"], &format!("{at}.start"))?, minutes(&o["end"], &format!("{at}.end"))?);
    if start >= end {
        return Err(format!("{at}.start must be before its end"));
    }
    Ok(format!("Tunable::Fixed(DaySpan::new(Duration::minutes({start}), Duration::minutes({end})))"))
}

fn look(v: &Value, at: &str) -> Result<String> {
    let o = object(v, at)?;
    match o.get("type").and_then(Value::as_str) {
        Some("sun-curve") => keys(o, &["type"], &[], at).map(|()| "Look::SunCurve".into()),
        Some("on-off") => keys(o, &["type"], &[], at).map(|()| "Look::OnOffOnly".into()),
        Some("work-curve") => {
            keys(o, &["type"], &["hours", "coolMireds", "warmMireds"], at)?;
            let default = |setting: &str| serde_json::json!({ "setting": setting });
            let hours = hours(o.get("hours").unwrap_or(&default("work")), &format!("{at}.hours"))?;
            let mireds = |field: &str, setting: &str| {
                tunable(o.get(field).unwrap_or(&default(setting)), &format!("{at}.{field}"), &[setting], |v, at| {
                    integer(v, at, 1, 1000).map(|n| n.to_string())
                })
            };
            let cool = mireds("coolMireds", "work_cool_mireds")?;
            let warm = mireds("warmMireds", "work_warm_mireds")?;
            Ok(format!("Look::WorkCurve(WorkCurve {{ hours: {hours}, cool: {cool}, warm: {warm} }})"))
        }
        _ => Err(format!("{at}.type is not a supported look")),
    }
}

fn rule(v: &Value, at: &str) -> Result<String> {
    let o = object(v, at)?;
    match o.get("type").and_then(Value::as_str) {
        Some("always") => keys(o, &["type"], &[], at).map(|()| "Rule::ALWAYS".into()),
        Some("window") => {
            keys(o, &["type", "hours"], &["days", "fadeMinutes"], at)?;
            let hours = hours(&o["hours"], &format!("{at}.hours"))?;
            let every = serde_json::json!({ "value": "every-day" });
            let days =
                tunable(o.get("days").unwrap_or(&every), &format!("{at}.days"), &["active_days"], |v, at| {
                    match v.as_str() {
                        Some("every-day") => Ok("EVERY_DAY".into()),
                        Some("weekdays") => Ok("MONDAY_TO_FRIDAY".into()),
                        _ => Err(format!("{at} must be every-day or weekdays")),
                    }
                })?;
            let zero = serde_json::json!({ "value": 0 });
            let fade =
                tunable(o.get("fadeMinutes").unwrap_or(&zero), &format!("{at}.fadeMinutes"), &["fade"], |v, at| {
                    integer(v, at, 0, 1440).map(|n| format!("Duration::minutes({n})"))
                })?;
            Ok(format!("Window {{ hours: {hours}, days: {days}, fade: {fade} }}.rule()"))
        }
        _ => Err(format!("{at}.type must be always or window")),
    }
}

fn mode(v: &Value, at: &str) -> Result<&'static str> {
    let name = string(v, at)?;
    MODES.iter().find(|(n, _)| *n == name).map(|(_, m)| *m).ok_or(format!("{at} names an unsupported mode {name:?}"))
}

fn names(v: &Value, at: &str) -> Result<String> {
    let list: Result<Vec<String>> = array(v, at)?
        .iter()
        .enumerate()
        .map(|(i, n)| string(n, &format!("{at}[{i}]")).map(|s| format!("{s:?}")))
        .collect();
    Ok(format!("&[{}]", list?.join(", ")))
}

fn zone(v: &Value, at: &str, seen: &mut HashSet<String>) -> Result<String> {
    let o = object(v, at)?;
    keys(o, &["name", "lights", "gate"], &["modes", "controls", "weather"], at)?;
    let name = string(&o["name"], &format!("{at}.name"))?;
    if name.is_empty() || !seen.insert(name.to_string()) {
        return Err(format!("{at}.name must be a non-empty unique string"));
    }
    let mut lights = Vec::new();
    for (i, light) in array(&o["lights"], &format!("{at}.lights"))?.iter().enumerate() {
        let at = format!("{at}.lights[{i}]");
        let l = object(light, &at)?;
        keys(l, &["selection", "look"], &["peakPercent"], &at)?;
        let selection = string(&l["selection"], &format!("{at}.selection"))?;
        let look = look(&l["look"], &format!("{at}.look"))?;
        let peak = l.get("peakPercent").map_or(Ok(100), |p| integer(p, &format!("{at}.peakPercent"), 1, 100))?;
        lights.push(format!("Shows::new({selection:?}, {look}).at({peak})"));
    }
    let gate = array(&o["gate"], &format!("{at}.gate"))?;
    if !(1..=4).contains(&gate.len()) {
        return Err(format!("{at}.gate must contain 1 to 4 rules"));
    }
    let gate: Result<Vec<String>> = gate.iter().enumerate().map(|(i, r)| rule(r, &format!("{at}.gate[{i}]"))).collect();
    let modes = match o.get("modes") {
        None => MODES.iter().map(|(_, m)| format!("Mode::{m}")).collect(),
        Some(list) => {
            let modes: Result<Vec<&str>> = array(list, &format!("{at}.modes"))?
                .iter()
                .enumerate()
                .map(|(i, m)| mode(m, &format!("{at}.modes[{i}]")))
                .collect();
            let modes = modes?;
            if modes.iter().collect::<HashSet<_>>().len() != modes.len() {
                return Err(format!("{at}.modes must be unique"));
            }
            modes.iter().map(|m| format!("Mode::{m}")).collect::<Vec<_>>()
        }
    };
    let mut controls = Vec::new();
    if let Some(list) = o.get("controls") {
        for (i, c) in array(list, &format!("{at}.controls"))?.iter().enumerate() {
            let at = format!("{at}.controls[{i}]");
            let c = object(c, &at)?;
            keys(c, &["button", "single", "double", "hold"], &[], &at)?;
            let press = |field: &str| -> Result<String> {
                match &c[field] {
                    Value::Null => Ok("None".into()),
                    m => mode(m, &format!("{at}.{field}")).map(|m| format!("Some(Mode::{m})")),
                }
            };
            controls.push(format!(
                "Controls {{ by: {:?}, single: {}, twice: {}, hold: {} }}",
                string(&c["button"], &format!("{at}.button"))?,
                press("single")?,
                press("double")?,
                press("hold")?
            ));
        }
    }
    let weather = match o.get("weather") {
        None => "None".into(),
        Some(w) => {
            let at = format!("{at}.weather");
            let w = object(w, &at)?;
            keys(w, &["precipitation", "alerts"], &[], &at)?;
            format!(
                "Some(Weather {{ precip: {}, alerts: {} }})",
                names(&w["precipitation"], &format!("{at}.precipitation"))?,
                names(&w["alerts"], &format!("{at}.alerts"))?
            )
        }
    };
    Ok(format!(
        "    ZoneSpec {{\n        name: {name:?},\n        lights: &[{}],\n        gate: &[{}],\n        modes: &[{}],\n        controls: &[{}],\n        weather: {weather},\n    }},\n",
        lights.join(", "),
        gate?.join(", "),
        modes.join(", "),
        controls.join(", ")
    ))
}

fn generate(document: &Value) -> Result<(String, String)> {
    let d = object(document, "the document")?;
    keys(d, &["schemaVersion", "groups", "zones"], &[], "the document")?;
    if d["schemaVersion"].as_i64() != Some(1) {
        return Err("schemaVersion must be 1".into());
    }
    let groups = array(&d["groups"], "groups")?;
    if groups.len() != GROUPS.len() {
        return Err(format!("groups must be exactly {}", GROUPS.map(|g| g.0).join(", ")));
    }
    let mut code = String::from("// Generated by ev/build.rs from config/installation.json. Do not edit.\n\n");
    let _ = writeln!(code, "/// The installation: what each group is and how many it has, in Group order.");
    let _ = writeln!(code, "pub const LAYOUT: [GroupSpec; {}] = [", GROUPS.len());
    for (i, (g, (name, kind))) in groups.iter().zip(GROUPS).enumerate() {
        let at = format!("groups[{i}]");
        let o = object(g, &at)?;
        keys(o, &["name", "kind", "count"], &[], &at)?;
        if o["name"].as_str() != Some(name) {
            return Err(format!("{at}.name must be {name}: the groups are fixed, in this order"));
        }
        let k = string(&o["kind"], &format!("{at}.kind"))?;
        if KINDS.iter().find(|(n, _)| *n == k).map(|(_, kd)| *kd) != Some(kind) {
            return Err(format!("{at}.kind does not match the device kind for {name}"));
        }
        let count = integer(&o["count"], &format!("{at}.count"), 1, 26)?;
        let _ = writeln!(
            code,
            "    GroupSpec {{ group: Group::{name}, name: {name:?}, kind: Kind::{kind}, count: {count} }},"
        );
    }
    code.push_str("];\n");
    let layout = code;
    let mut code = String::from("// Generated by ev/build.rs from config/installation.json. Do not edit.\n\n");
    let zones = array(&d["zones"], "zones")?;
    if !(1..=8).contains(&zones.len()) {
        return Err("zones must contain 1 to 8 entries".into());
    }
    let mut seen = HashSet::new();
    let _ = writeln!(code, "/// Lights that share a mode and a gate. Every light is in exactly one.");
    let _ = writeln!(code, "pub const ZONES: [ZoneSpec; {}] = [", zones.len());
    for (i, z) in zones.iter().enumerate() {
        code.push_str(&zone(z, &format!("zones[{i}]"), &mut seen)?);
    }
    code.push_str("];\n");
    Ok((layout, code))
}
