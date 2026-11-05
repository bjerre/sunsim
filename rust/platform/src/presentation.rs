//! What the status display says, for any renderer: the board's LCD
//! (display.rs) and the browser simulator draw the same text, tones, sky,
//! groups and links, each in its own layout and colors; and the program
//! flow the browser shows: each zone's mode, gate and lights. Pure: from a
//! StatusSnapshot (and the installation).

use ev::*;

use crate::console::{clock_text, duration_text};

/// What a piece of text means, for the renderer to color.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tone {
    Muted,
    Daylight,
    Tuning,
    Advisory,
    Watch,
    Warning,
    Sky,
}

impl Tone {
    /// As the web knows it.
    pub const fn name(self) -> &'static str {
        match self {
            Tone::Muted => "muted",
            Tone::Daylight => "daylight",
            Tone::Tuning => "tuning",
            Tone::Advisory => "advisory",
            Tone::Watch => "watch",
            Tone::Warning => "warning",
            Tone::Sky => "sky",
        }
    }
}

/// A group's letter on the display: D = the daylight wall, W = the work
/// light, L = the desk downlight, G = the grow light, B = the button.
pub const fn group_label(group: Group) -> &'static str {
    match group {
        Group::Wall => "D",
        Group::Work => "W",
        Group::Desk => "L",
        Group::Grow => "G",
        Group::Button => "B",
    }
}

const WEEKDAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

/// The status display's content.
#[derive(Clone, PartialEq, Debug)]
pub struct StatusView {
    pub time_valid: bool,
    pub time: String,
    pub weekday: String,
    pub day_length: String,
    pub next_event: String,
    pub sunrise: String, // "Rise 06:37"
    pub sunset: String,  // "Set 16:47"
    pub sun_azimuth_deg: f64,
    pub sun_elevation_deg: f64,
    pub cloud_percent: Option<u8>,
    pub precipitation: Precip,
    pub mode: &'static str,
    pub mode_tone: Tone,
    pub sky: String,
    pub sky_tone: Tone,
    pub links: PerRole<DeviceLink>,
}

impl StatusView {
    pub fn of(st: &StatusSnapshot) -> StatusView {
        let day = &st.day;
        let (sunrise, sunset) = if st.time.valid {
            (format!("Rise {}", clock_text(day.sunrise)), format!("Set {}", clock_text(day.sunset)))
        } else {
            ("Rise --:--".into(), "Set --:--".into())
        };
        let (time, weekday, day_length, next_event) = if st.time.valid {
            let minutes = st.time.since_midnight().whole_minutes();
            (
                format!("{:02}:{:02}", minutes / 60, minutes % 60),
                format!("{}{}", if st.time.simulated { "sim " } else { "" }, WEEKDAYS[st.time.weekday().0 as usize]),
                format!("Day {}", duration_text(day.day_length)),
                format!(
                    "{} {}  {}",
                    if day.next_is_sunrise { "Rise" } else { "Set" },
                    duration_text(day.until_next),
                    clock_text(day.next_event)
                ),
            )
        } else {
            ("--:--".into(), String::new(), "Waiting for".into(), "time sync...".into())
        };

        // The first zone's mode: the main one.
        let zone = st.zones[ZoneId(0)];
        let (mode, mode_tone) = match zone.mode {
            Mode::Daylight if zone.gate_open => ("daylight", Tone::Daylight),
            Mode::Daylight => ("idle", Tone::Muted),
            Mode::Off => ("off", Tone::Muted),
            Mode::RedAlert => ("RED ALERT", Tone::Warning),
            Mode::Tuning => ("tuning", Tone::Tuning),
        };

        // The sky: the worst alert in its tone, else what's falling, else cloud.
        let (sky, sky_tone) = match (st.sky.alert(), st.sky.conditions) {
            (AlertLevel::Warning, _) => (AlertLevel::Warning.name().into(), Tone::Warning),
            (AlertLevel::Watch, _) => (AlertLevel::Watch.name().into(), Tone::Watch),
            (AlertLevel::Advisory, _) => (AlertLevel::Advisory.name().into(), Tone::Advisory),
            (AlertLevel::None, Some(c)) if c.precip != Precip::None => (c.precip.name().into(), Tone::Sky),
            (AlertLevel::None, _) => (st.sky.cloud_percent.map_or(String::new(), |c| format!("cloud {c}%")), Tone::Sky),
        };

        let mut links = PerRole::filled(DeviceLink::Unpaired);
        for role in ROLES {
            links[role] = st.devices[role].link;
        }
        StatusView {
            time_valid: st.time.valid,
            time,
            weekday,
            day_length,
            next_event,
            sunrise,
            sunset,
            sun_azimuth_deg: if st.time.valid { day.sun_azimuth_deg } else { 180.0 },
            sun_elevation_deg: if st.time.valid { day.sun_elevation_deg } else { -90.0 },
            cloud_percent: st.sky.cloud_percent,
            precipitation: st.sky.conditions.map_or(Precip::None, |c| c.precip),
            mode,
            mode_tone,
            sky,
            sky_tone,
            links,
        }
    }
}

// ---- The program flow ------------------------------------------------------

/// A mode a zone can run, and the button press that picks it, if any.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ModeChoice {
    pub mode: Mode,
    pub button_press: Option<Press>,
}

/// A light in a zone, and what it shows.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct FlowMember {
    pub role: Role,
    pub behavior: &'static str, // "sun curve", "work curve", "on/off"
}

/// A zone as the program flow shows it: its mode, its gate (which only the
/// daylight program heeds), what it can run, and its lights.
#[derive(Clone, PartialEq, Debug)]
pub struct FlowZone {
    pub name: String,
    pub mode: Mode,
    pub gate_open: bool,
    pub gate_applies: bool,
    pub available_modes: Vec<ModeChoice>,
    pub members: Vec<FlowMember>,
}

/// What a look does, in words.
pub const fn behavior_name(look: &Look) -> &'static str {
    match look {
        Look::SunCurve => "sun curve",
        Look::WorkCurve(_) => "work curve",
        Look::OnOffOnly => "on/off",
    }
}

/// Each zone of `installation`, as the status has it.
pub fn program_flow(st: &StatusSnapshot, installation: &Installation) -> Vec<FlowZone> {
    let t = &installation.topology;
    installation
        .zone_ids()
        .map(|id| {
            let zone = installation.spec(id);
            let status = st.zones[id];
            let available_modes = zone
                .modes
                .iter()
                .map(|&mode| {
                    let button_press = zone.controls.iter().find_map(|c| {
                        if c.single == Some(mode) {
                            Some(Press::Single)
                        } else if c.twice == Some(mode) {
                            Some(Press::Double)
                        } else if c.hold == Some(mode) {
                            Some(Press::Hold)
                        } else {
                            None
                        }
                    });
                    ModeChoice { mode, button_press }
                })
                .collect();
            let members = lights()
                .filter(|&r| t.zone_of[r] == Some(id))
                .map(|role| FlowMember { role, behavior: behavior_name(&t.look_of[role]) })
                .collect();
            FlowZone {
                name: zone.name.clone(),
                mode: status.mode,
                gate_open: status.gate_open,
                gate_applies: status.mode == Mode::Daylight,
                available_modes,
                members,
            }
        })
        .collect()
}
