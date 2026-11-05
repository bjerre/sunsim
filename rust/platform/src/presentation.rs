//! What the status display says, for any renderer: the board's LCD
//! (display.rs) and the browser simulator draw the same text, tones, groups
//! and links, each in its own layout and colors. Pure: from a
//! StatusSnapshot.

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
    pub time: String,
    pub weekday: String,
    pub day_length: String,
    pub next_event: String,
    pub mode: &'static str,
    pub mode_tone: Tone,
    pub sky: String,
    pub sky_tone: Tone,
    pub links: PerRole<DeviceLink>,
}

impl StatusView {
    pub fn of(st: &StatusSnapshot) -> StatusView {
        let (time, weekday, day_length, next_event) = if st.time.valid {
            let minutes = st.time.since_midnight().whole_minutes();
            let day = &st.day;
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
        StatusView { time, weekday, day_length, next_event, mode, mode_tone, sky, sky_tone, links }
    }
}
