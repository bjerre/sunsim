//! Installation profiles: a different grouping and schedule of the same
//! lights, as data the app loads at run time (the board from NVS, uploaded
//! over the console; the browser simulator from a JSON file), checked as it
//! loads. Profiles keep the build's role catalogue: which devices exist and
//! what they are stay fixed by the firmware. A port of the C++
//! installation_profile.cppm.
//!
//! ```json
//! {"schemaVersion": 1, "zones": [
//!   {"name": "studio", "startMinute": 420, "endMinute": 1380, "buttonRole": "Button",
//!    "members": [{"role": "WallA", "look": "sun-curve"}, {"role": "Work", "look": "work-curve", "peakPercent": 90}],
//!    "weather": {"precipitation": ["WallA"], "alerts": []}}]}
//! ```

use crate::installation::{Installation, MAX_ZONES, Zone};
use crate::json::{self, Value};
use crate::time::Duration;
use crate::topology::*;
use crate::types::*;

/// The longest zone name.
pub const MAX_NAME: usize = 31;

/// What a member shows, by the index the browser passes: sun-curve,
/// work-curve, on-off.
pub const LOOKS: [&str; 3] = ["sun-curve", "work-curve", "on-off"];

/// Builds an Installation a step at a time, each step checked; finish()
/// checks the whole. (The browser simulator calls these steps one by one;
/// parse_profile() calls them from a document.)
#[derive(Default)]
pub struct ProfileBuilder {
    zones: Vec<Option<Zone>>,
    topology: Option<Topology>,
    member_zone: PerRole<Option<ZoneId>>,
    button_assigned: bool,
    ready: Option<Installation>,
}

const fn empty_topology() -> Topology {
    Topology {
        zone_of: PerRole::filled(None),
        look_of: PerRole::filled(Look::SunCurve),
        peak_of: PerRole::filled(100),
        controls_of: PerRole::filled(None),
        weather: WeatherLights { precip: RoleSet::EMPTY, alerts: RoleSet::EMPTY },
    }
}

impl ProfileBuilder {
    pub fn new() -> Self {
        ProfileBuilder::default()
    }

    /// A profile of `zone_count` zones (1-8), starting over.
    pub fn begin(&mut self, zone_count: i32) -> bool {
        if !(1..=MAX_ZONES as i32).contains(&zone_count) {
            return false;
        }
        *self = ProfileBuilder {
            zones: vec![None; zone_count as usize],
            topology: Some(empty_topology()),
            ..ProfileBuilder::default()
        };
        true
    }

    fn zone_index(&self, index: i32) -> Option<usize> {
        (self.topology.is_some() && (0..self.zones.len() as i32).contains(&index)).then_some(index as usize)
    }

    /// Zone `index`: its name (unique, 1-31 bytes) and daily hours, every
    /// day, in minutes after midnight. It can run every mode.
    pub fn set_zone(&mut self, index: i32, name: &str, start_minute: i32, end_minute: i32) -> bool {
        let Some(i) = self.zone_index(index) else { return false };
        if name.is_empty()
            || name.len() > MAX_NAME
            || !(0..end_minute).contains(&start_minute)
            || end_minute > 24 * 60
            || self.zones.iter().enumerate().any(|(j, z)| j != i && z.as_ref().is_some_and(|z| z.name == name))
        {
            return false;
        }
        let hours = DaySpan::new(Duration::minutes(start_minute as i64), Duration::minutes(end_minute as i64));
        self.zones[i] = Some(Zone {
            name: name.to_string(),
            gate: vec![Window::hours(Tunable::Fixed(hours)).rule()],
            modes: MODES.to_vec(),
            controls: Vec::new(),
        });
        true
    }

    /// A light (by its slot) in zone `zone`, with a look (LOOKS' index) its
    /// kind takes, and a peak of 1-100%. Each light in one zone only.
    pub fn add_member(&mut self, zone: i32, role_index: i32, look_index: i32, peak_percent: i32) -> bool {
        let (Some(z), Some(role)) = (self.zone_index(zone), role_at(role_index)) else { return false };
        let t = self.topology.as_mut().expect("begun");
        if self.zones[z].is_none() || !is_light(role) || t.zone_of[role].is_some() || !(1..=100).contains(&peak_percent)
        {
            return false;
        }
        let look = match (look_index, kind_of(role)) {
            (0, Kind::ColorBulb) => Look::SunCurve,
            (1, Kind::ColorBulb) => Look::WorkCurve(WorkCurve::SETTINGS),
            (2, Kind::Plug) => Look::OnOffOnly,
            _ => return false,
        };
        t.zone_of[role] = Some(ZoneId(z as u8));
        t.look_of[role] = look;
        t.peak_of[role] = peak_percent as u8;
        self.member_zone[role] = Some(ZoneId(z as u8));
        true
    }

    /// The button (by its slot) chooses zone `zone`'s mode: single press
    /// daylight, double red alert, hold off. One zone only.
    pub fn set_button(&mut self, zone: i32, role_index: i32) -> bool {
        let (Some(z), Some(role)) = (self.zone_index(zone), role_at(role_index)) else { return false };
        if self.zones[z].is_none() || !is_button(role) || self.button_assigned {
            return false;
        }
        let controls = Controls::by(role_name(role));
        self.zones[z].as_mut().expect("set").controls.push(controls);
        let t = self.topology.as_mut().expect("begun");
        t.controls_of[role] = Some(controls);
        t.zone_of[role] = Some(ZoneId(z as u8));
        self.button_assigned = true;
        true
    }

    /// A color light already in zone `zone` pulses for the weather: 0 rain
    /// or snow, 1 alerts.
    pub fn add_weather_role(&mut self, zone: i32, role_index: i32, effect: i32) -> bool {
        let (Some(z), Some(role)) = (self.zone_index(zone), role_at(role_index)) else { return false };
        if self.member_zone[role] != Some(ZoneId(z as u8)) || !caps(role).color {
            return false;
        }
        let w = &mut self.topology.as_mut().expect("begun").weather;
        let set = match effect {
            0 => &mut w.precip,
            1 => &mut w.alerts,
            _ => return false,
        };
        if set.contains(role) {
            return false;
        }
        set.insert(role);
        true
    }

    /// The profile, if every zone is set and has a light, and every light is
    /// in one.
    pub fn finish(&mut self) -> bool {
        let Some(t) = self.topology else { return false };
        if lights().any(|r| t.zone_of[r].is_none())
            || self
                .zones
                .iter()
                .enumerate()
                .any(|(i, z)| z.is_none() || !lights().any(|r| t.zone_of[r] == Some(ZoneId(i as u8))))
        {
            return false;
        }
        self.ready = Some(Installation { zones: self.zones.drain(..).flatten().collect(), topology: t });
        self.topology = None;
        true
    }

    /// The finished profile.
    pub fn installation(&self) -> Option<&Installation> {
        self.ready.as_ref()
    }

    pub fn into_installation(self) -> Option<Installation> {
        self.ready
    }
}

fn role_at(index: i32) -> Option<Role> {
    usize::try_from(index).ok().and_then(|i| ROLES.get(i).copied())
}

// An object's members, each named in `allowed` and none twice.
fn members<'a>(v: &'a Value, allowed: &[&str], at: &str) -> Result<&'a [(String, Value)], String> {
    let o = v.as_object().ok_or(format!("{at} must be an object"))?;
    for (i, (key, _)) in o.iter().enumerate() {
        if !allowed.contains(&key.as_str()) {
            return Err(format!("{at} has an unknown field {key}"));
        }
        if o[..i].iter().any(|(k, _)| k == key) {
            return Err(format!("{at} has {key} twice"));
        }
    }
    Ok(o)
}

fn field<'a>(o: &'a [(String, Value)], key: &str) -> Option<&'a Value> {
    o.iter().find(|(k, _)| k == key).map(|(_, v)| v)
}

fn role_index(v: &Value, at: &str) -> Result<i32, String> {
    let name = v.as_str().ok_or(format!("{at} must be a role name"))?;
    let role = parse_role(name).ok_or(format!("{at} names no role: {name:?}"))?;
    Ok(slot(role) as i32)
}

/// A profile document, checked, as the Installation it describes; or why not.
pub fn parse_profile(text: &str) -> Result<Installation, String> {
    let doc = json::parse(text)?;
    let d = members(&doc, &["schemaVersion", "zones"], "the profile")?;
    if field(d, "schemaVersion").and_then(Value::as_int) != Some(1) {
        return Err("schemaVersion must be 1".into());
    }
    let zones = field(d, "zones").and_then(Value::as_array).ok_or("zones must be an array")?;
    let mut b = ProfileBuilder::new();
    if !b.begin(zones.len() as i32) {
        return Err(format!("a profile has 1 to {MAX_ZONES} zones"));
    }
    for (i, zone) in zones.iter().enumerate() {
        let at = format!("zones[{i}]");
        let z = members(zone, &["name", "startMinute", "endMinute", "members", "buttonRole", "weather"], &at)?;
        let name = field(z, "name").and_then(Value::as_str).ok_or(format!("{at}.name must be a string"))?;
        let minute =
            |key: &str| field(z, key).and_then(Value::as_int).ok_or(format!("{at}.{key} must be a whole number"));
        let (start, end) = (minute("startMinute")?, minute("endMinute")?);
        if !b.set_zone(i as i32, name, start, end) {
            return Err(format!(
                "{at}: needs a unique name of 1-{MAX_NAME} bytes, and 0 <= startMinute < endMinute <= 1440"
            ));
        }
        let list = field(z, "members").and_then(Value::as_array).ok_or(format!("{at}.members must be an array"))?;
        for (j, member) in list.iter().enumerate() {
            let at = format!("{at}.members[{j}]");
            let m = members(member, &["role", "look", "peakPercent"], &at)?;
            let role = role_index(field(m, "role").ok_or(format!("{at} needs a role"))?, &format!("{at}.role"))?;
            let look = field(m, "look").and_then(Value::as_str).ok_or(format!("{at}.look must be a string"))?;
            let look = LOOKS
                .iter()
                .position(|l| *l == look)
                .ok_or(format!("{at}.look must be one of {}", LOOKS.join(", ")))?;
            let peak = field(m, "peakPercent")
                .map_or(Some(100), Value::as_int)
                .ok_or(format!("{at}.peakPercent must be a whole number"))?;
            if !b.add_member(i as i32, role, look as i32, peak) {
                return Err(format!(
                    "{at}: each light in one zone only, with a look its kind takes (plugs: on-off), and a peak of 1-100%"
                ));
            }
        }
        if let Some(button) = field(z, "buttonRole")
            && !b.set_button(i as i32, role_index(button, &format!("{at}.buttonRole"))?)
        {
            return Err(format!("{at}.buttonRole: the button, and in one zone only"));
        }
        if let Some(weather) = field(z, "weather") {
            let w = members(weather, &["precipitation", "alerts"], &format!("{at}.weather"))?;
            for (effect, key) in ["precipitation", "alerts"].into_iter().enumerate() {
                let Some(list) = field(w, key) else { continue };
                let list = list.as_array().ok_or(format!("{at}.weather.{key} must be an array"))?;
                for (k, role) in list.iter().enumerate() {
                    let at = format!("{at}.weather.{key}[{k}]");
                    if !b.add_weather_role(i as i32, role_index(role, &at)?, effect as i32) {
                        return Err(format!("{at}: a color light in this zone, once"));
                    }
                }
            }
        }
    }
    if !b.finish() {
        return Err("every light needs a zone, and every zone a light".into());
    }
    Ok(b.into_installation().expect("finished"))
}
