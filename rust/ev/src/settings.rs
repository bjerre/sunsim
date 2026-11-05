//! The program settings people change (console `config <name> <value>`) and
//! the device keeps (NVS, a key per field), as one table: the console's
//! parsing, printing and help and the store's keys all come from it, so a
//! new setting is a ProgramConfig field and a line here.

use crate::field;
use crate::time::{Duration, Weekday};
use crate::topology::Field;
use crate::types::*;

/// What kind of value a setting holds: how it's typed, shown and stored.
#[derive(Clone, Copy)]
pub enum Kind {
    /// on | off; stored as a u8
    OnOff { field: Field<bool>, key: &'static str },
    /// a whole number in a range; stored as a u8
    Number { field: Field<u8>, min: u8, max: u8, unit: &'static str, key: &'static str },
    /// a duration; stored as whole minutes, a u16
    Minutes { field: Field<Duration>, max: Duration, key: &'static str },
    /// HH:MM-HH:MM; each end stored as minutes after midnight, a u16
    Span { field: Field<DaySpan>, start_key: &'static str, end_key: &'static str },
    /// mon-fri | all | mon,wed,...; stored as Weekdays' bits, a u8
    Days { field: Field<Weekdays>, key: &'static str },
}

#[derive(Clone, Copy)]
pub struct Setting {
    pub name: &'static str, // `config <name> <value>`
    pub kind: Kind,
    pub help: &'static str,
}

pub const SETTINGS: &[Setting] = &[
    Setting {
        name: "hours",
        kind: Kind::Span { field: field!(active), start_key: "act.start", end_key: "act.end" },
        help: "when the lights may be on",
    },
    Setting {
        name: "days",
        kind: Kind::Days { field: field!(active_days), key: "act.days" },
        help: "the days they may be on",
    },
    Setting {
        name: "fade",
        kind: Kind::Minutes { field: field!(fade), max: Duration::minutes(180), key: "act.fade" },
        help: "fade in and out at each edge of the hours",
    },
    Setting {
        name: "workhours",
        kind: Kind::Span { field: field!(work), start_key: "work.start", end_key: "work.end" },
        help: "the work light: cool -> warm across these",
    },
    Setting {
        name: "sunshift",
        kind: Kind::Number { field: field!(sun_shift_percent), min: 0, max: 100, unit: "%", key: "sun.shift" },
        help: "the wall dims away from the sun by up to this (0: off)",
    },
    Setting {
        name: "amber",
        kind: Kind::OnOff { field: field!(amber_enabled), key: "sun.amber" },
        help: "an amber glow while the sun is low",
    },
    Setting {
        name: "ambersweep",
        kind: Kind::Number {
            field: field!(amber_sweep_deg),
            min: 0,
            max: MAX_AMBER_SWEEP_DEG,
            unit: " deg",
            key: "sun.sweep",
        },
        help: "the glow follows the sun: the far end lags this much (0: all alike)",
    },
    Setting {
        name: "clouds",
        kind: Kind::OnOff { field: field!(clouds_enabled), key: "sky.on" },
        help: "cloud cover shapes the wall (off: always clear)",
    },
    Setting {
        name: "clouddim",
        kind: Kind::Number { field: field!(cloud_dim_percent), min: 0, max: 100, unit: "%", key: "sky.dim" },
        help: "full overcast: up to this much dimmer",
    },
    Setting {
        name: "cloudcool",
        kind: Kind::Number { field: field!(cloud_cool_percent), min: 0, max: 100, unit: "%", key: "sky.cool" },
        help: "... and this far to the cool end",
    },
    Setting {
        name: "weather",
        kind: Kind::OnOff { field: field!(weather_enabled), key: "wx.on" },
        help: "the weather lights (each zone declares which)",
    },
    Setting {
        name: "syncwindow",
        kind: Kind::Span { field: field!(sync), start_key: "sync.start", end_key: "sync.end" },
        help: "the daily clock and forecast sync",
    },
];

pub fn find_setting(name: &str) -> Option<&'static Setting> {
    SETTINGS.iter().find(|s| s.name == name)
}

impl Kind {
    /// Its NVS keys: one, or a span's two.
    pub const fn keys(&self) -> (&'static str, Option<&'static str>) {
        match *self {
            Kind::OnOff { key, .. } | Kind::Number { key, .. } | Kind::Minutes { key, .. } | Kind::Days { key, .. } => {
                (key, None)
            }
            Kind::Span { start_key, end_key, .. } => (start_key, Some(end_key)),
        }
    }
}

/// Every NVS key the table uses.
pub fn setting_keys() -> Vec<&'static str> {
    SETTINGS
        .iter()
        .flat_map(|s| {
            let (key, second) = s.kind.keys();
            std::iter::once(key).chain(second)
        })
        .collect()
}

// Distinct names, distinct NVS keys, and keys within NVS's 15 characters.
const _: () = {
    let mut keys: [&str; 2 * SETTINGS.len()] = [""; 2 * SETTINGS.len()];
    let mut n = 0;
    let mut i = 0;
    while i < SETTINGS.len() {
        let (key, second) = SETTINGS[i].kind.keys();
        keys[n] = key;
        n += 1;
        if let Some(k) = second {
            keys[n] = k;
            n += 1;
        }
        let mut j = 0;
        while j < i {
            assert!(!str_eq(SETTINGS[i].name, SETTINGS[j].name), "settings need distinct names");
            j += 1;
        }
        i += 1;
    }
    let mut a = 0;
    while a < n {
        assert!(keys[a].len() <= 15, "NVS keys are at most 15 characters");
        let mut b = 0;
        while b < a {
            assert!(!str_eq(keys[a], keys[b]), "settings need distinct NVS keys");
            b += 1;
        }
        a += 1;
    }
};

const DAY_NAMES: [&str; 7] = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"];

/// Monday first, as they're typed and shown.
const WEEK: [Weekday; 7] = [
    Weekday::MONDAY,
    Weekday::TUESDAY,
    Weekday::WEDNESDAY,
    Weekday::THURSDAY,
    Weekday::FRIDAY,
    Weekday::SATURDAY,
    Weekday::SUNDAY,
];

fn day_name(day: Weekday) -> &'static str {
    DAY_NAMES[day.0 as usize]
}

/// A whole number, all of `text` (an optional '-', then digits).
fn whole_number(text: &str) -> Option<i64> {
    if text.starts_with('+') {
        return None;
    }
    text.parse().ok()
}

/// "HH:MM" as a time after midnight (24:00 allowed).
fn clock_time(text: &str) -> Option<Duration> {
    let colon = text.find(':')?;
    if text.len() - colon != 3 {
        return None;
    }
    let h = whole_number(&text[..colon])?;
    let m = whole_number(&text[colon + 1..])?;
    if h < 0 || !(0..=59).contains(&m) {
        return None;
    }
    Some(Duration::hours(h) + Duration::minutes(m))
}

fn clock_text(t: Duration) -> String {
    let minutes = t.whole_minutes();
    format!("{:02}:{:02}", minutes / 60, minutes % 60)
}

fn parse_days(text: &str) -> Option<Weekdays> {
    match text {
        "mon-fri" => return Some(MONDAY_TO_FRIDAY),
        "all" => return Some(EVERY_DAY),
        _ => {}
    }
    let mut days = Weekdays::default();
    for name in text.split(',') {
        days.insert(*WEEK.iter().find(|&&d| day_name(d) == name)?);
    }
    Some(days)
}

fn days_text(days: Weekdays) -> String {
    if days == MONDAY_TO_FRIDAY {
        return "mon-fri".into();
    }
    if days == EVERY_DAY {
        return "all".into();
    }
    let names: Vec<_> = WEEK.iter().filter(|&&d| days.contains(d)).map(|&d| day_name(d)).collect();
    if names.is_empty() { "none".into() } else { names.join(",") }
}

impl Setting {
    /// Sets it in `config` from what was typed, or says why not.
    pub fn set(&self, config: &mut ProgramConfig, text: &str) -> Result<(), String> {
        let ok = match self.kind {
            Kind::OnOff { field, .. } => match text {
                "on" | "off" => {
                    (field.set)(config, text == "on");
                    true
                }
                _ => false,
            },
            Kind::Number { field, min, max, .. } => match whole_number(text) {
                Some(v) if (min as i64..=max as i64).contains(&v) => {
                    (field.set)(config, v as u8);
                    true
                }
                _ => false,
            },
            Kind::Minutes { field, max, .. } => match whole_number(text) {
                Some(v) if v >= 0 && Duration::minutes(v) <= max => {
                    (field.set)(config, Duration::minutes(v));
                    true
                }
                _ => false,
            },
            Kind::Span { field, .. } => {
                let (start, end) = text.split_once('-').map_or((text, None), |(s, e)| (s, Some(e)));
                match (clock_time(start), end.and_then(clock_time)) {
                    (Some(start), Some(end)) if DaySpan::new(start, end).valid() => {
                        (field.set)(config, DaySpan::new(start, end));
                        true
                    }
                    _ => false,
                }
            }
            Kind::Days { field, .. } => match parse_days(text) {
                Some(days) if days.bits != 0 => {
                    (field.set)(config, days);
                    true
                }
                _ => false,
            },
        };
        if ok { Ok(()) } else { Err(format!("{} takes {}", self.name, self.syntax())) }
    }

    /// Its value in `config`, as set() takes it (plus a unit).
    pub fn show(&self, c: &ProgramConfig) -> String {
        match self.kind {
            Kind::OnOff { field, .. } => (if (field.get)(c) { "on" } else { "off" }).into(),
            Kind::Number { field, unit, .. } => format!("{}{}", (field.get)(c), unit),
            Kind::Minutes { field, .. } => format!("{} min", (field.get)(c).whole_minutes()),
            Kind::Span { field, .. } => {
                let span = (field.get)(c);
                format!("{}-{}", clock_text(span.start), clock_text(span.end))
            }
            Kind::Days { field, .. } => days_text((field.get)(c)),
        }
    }

    /// What set() takes: "on|off", "0-100", "HH:MM-HH:MM", ...
    pub fn syntax(&self) -> String {
        match self.kind {
            Kind::OnOff { .. } => "on|off".into(),
            Kind::Number { min, max, .. } => format!("{min}-{max}"),
            Kind::Minutes { max, .. } => format!("0-{} (minutes)", max.whole_minutes()),
            Kind::Span { .. } => "HH:MM-HH:MM".into(),
            Kind::Days { .. } => "mon-fri|all|mon,tue,...".into(),
        }
    }
}
