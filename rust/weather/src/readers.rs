//! Readers for the weather services' replies, fed as they stream in.

use ev::{
    AlertLevel, Alerts, Conditions, Duration, Instant, Intensity, Precip, SysSeconds, days_from_civil, is_valid_date,
};

use crate::json::{JsonScanner, JsonSink, Scalar};

// ---- What the codes mean ---------------------------------------------------

/// What's falling for a WMO weather code (Open-Meteo's weather_code); None
/// for clear, cloud and fog.
pub const fn precip_of(wmo_code: i32) -> (Precip, Intensity) {
    use Intensity::*;
    use Precip::*;
    match wmo_code {
        51 => (Drizzle, Light),
        53 => (Drizzle, Moderate),
        55 => (Drizzle, Heavy),
        56 => (FreezingRain, Light), // freezing drizzle
        57 => (FreezingRain, Moderate),
        61 | 80 => (Rain, Light), // 80-82: showers
        63 | 81 => (Rain, Moderate),
        65 | 82 => (Rain, Heavy),
        66 => (FreezingRain, Light),
        67 => (FreezingRain, Heavy),
        71 | 77 | 85 => (Snow, Light), // 77: snow grains; 85-86: showers
        73 => (Snow, Moderate),
        75 | 86 => (Snow, Heavy),
        95 => (Thunderstorm, Moderate),
        96 | 99 => (Thunderstorm, Heavy), // with hail
        _ => (None, Light),
    }
}

/// An NWS alert's tier, by its event name's last word ("Tornado Watch");
/// failing that, Extreme severity is a warning and Severe a watch. Other
/// statements and outlooks: None.
pub fn alert_level_of(event: &str, severity: &str) -> AlertLevel {
    if event.ends_with("Warning") || event.ends_with("Emergency") {
        AlertLevel::Warning
    } else if event.ends_with("Watch") {
        AlertLevel::Watch
    } else if event.ends_with("Advisory") {
        AlertLevel::Advisory
    } else if severity == "Extreme" {
        AlertLevel::Warning
    } else if severity == "Severe" {
        AlertLevel::Watch
    } else {
        AlertLevel::None
    }
}

// A whole number of 1 to `max` digits at the start of `text`, and the rest.
fn digits(text: &str, max: usize) -> Option<(i64, &str)> {
    let n = text.bytes().take(max).take_while(u8::is_ascii_digit).count();
    if n == 0 {
        return None;
    }
    Some((text[..n].parse().ok()?, &text[n..]))
}

// `text` without its first character, if that's `c`.
fn expect(text: &str, c: char) -> Option<&str> {
    text.strip_prefix(c)
}

/// An ISO 8601 time with its offset, as NWS gives them:
/// "2026-10-16T18:00:00-05:00" (or "...Z").
pub fn parse_iso_time(text: &str) -> Option<SysSeconds> {
    let (y, rest) = digits(text, 4)?;
    let (mo, rest) = digits(expect(rest, '-')?, 2)?;
    let (d, rest) = digits(expect(rest, '-')?, 2)?;
    let (h, rest) = digits(expect(rest, 'T')?, 2)?;
    let (mi, rest) = digits(expect(rest, ':')?, 2)?;
    let (sec, zone) = digits(expect(rest, ':')?, 2)?;
    if !is_valid_date(y as i32, mo as u32, d as u32) {
        return None;
    }
    let t = SysSeconds(days_from_civil(y as i32, mo as u32, d as u32) * 86_400 + h * 3600 + mi * 60 + sec);
    if zone == "Z" {
        return Some(t);
    }
    let sign = zone.chars().next().filter(|&c| c == '+' || c == '-')?;
    let (oh, rest) = digits(&zone[1..], 2)?;
    let (om, rest) = digits(expect(rest, ':')?, 2)?;
    if zone.len() != 6 || !rest.is_empty() {
        return None;
    }
    let offset = Duration::hours(oh) + Duration::minutes(om);
    Some(if sign == '-' { t + offset } else { t - offset }) // local = UTC + offset
}

// ---- Open-Meteo -----------------------------------------------------------

/// An hour of the forecast.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Hour {
    pub start: SysSeconds,
    pub cloud_percent: u8,
}

#[derive(Default, Debug)]
struct Hourly {
    times: Vec<Option<f64>>,
    covers: Vec<Option<f64>>,
}

impl JsonSink for Hourly {
    fn scalar(&mut self, s: &Scalar) {
        if s.parent != "hourly" {
            return;
        }
        match s.key {
            "time" => self.times.push(s.number()),
            "cloud_cover" => self.covers.push(s.number()), // null: None
            _ => {}
        }
    }
}

/// The hourly forecast: `hourly=cloud_cover&timeformat=unixtime`.
#[derive(Default, Debug)]
pub struct ForecastReader {
    json: JsonScanner,
    hourly: Hourly,
}

impl ForecastReader {
    pub fn new() -> Self {
        ForecastReader::default()
    }

    pub fn feed(&mut self, chunk: &[u8]) {
        self.json.feed(chunk, &mut self.hourly);
    }

    /// The hours with data, or empty if the reply wasn't a forecast.
    pub fn hours(&self) -> Vec<Hour> {
        let Hourly { times, covers } = &self.hourly;
        if times.len() != covers.len() {
            return Vec::new();
        }
        times
            .iter()
            .zip(covers)
            .filter_map(|(t, c)| {
                Some(Hour {
                    start: SysSeconds(t.as_ref()?.round() as i64),
                    cloud_percent: c.as_ref()?.round().clamp(0.0, 100.0) as u8,
                })
            })
            .collect()
    }
}

#[derive(Default, Debug)]
struct Current {
    cloud: Option<f64>,
    code: Option<f64>,
}

impl JsonSink for Current {
    fn scalar(&mut self, s: &Scalar) {
        if s.parent != "current" {
            return; // also "current_units", with the same keys
        }
        match s.key {
            "cloud_cover" => self.cloud = s.number(),
            "weather_code" => self.code = s.number(),
            _ => {}
        }
    }
}

/// The weather now: `current=cloud_cover,weather_code`.
#[derive(Default, Debug)]
pub struct ConditionsReader {
    json: JsonScanner,
    current: Current,
}

impl ConditionsReader {
    pub fn new() -> Self {
        ConditionsReader::default()
    }

    pub fn feed(&mut self, chunk: &[u8]) {
        self.json.feed(chunk, &mut self.current);
    }

    pub fn conditions(&self) -> Option<Conditions> {
        let (cloud, code) = (self.current.cloud?, self.current.code?);
        let (precip, intensity) = precip_of(code.round() as i32);
        Some(Conditions { cloud_percent: cloud.round().clamp(0.0, 100.0) as u8, precip, intensity })
    }
}

// ---- NWS ------------------------------------------------------------------

/// An NWS alert, as read.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Alert {
    pub level: AlertLevel,
    pub event: String,
    pub ends: Option<SysSeconds>, // `ends`, else `expires`
}

#[derive(Default, Debug)]
struct Fields {
    event: String,
    severity: String,
    status: String,
    message_type: String,
    ends: String,
    expires: String,
}

#[derive(Default, Debug)]
struct Features {
    fields: Fields,
    alerts: Vec<Alert>,
}

impl JsonSink for Features {
    fn scalar(&mut self, s: &Scalar) {
        if s.parent != "properties" || !s.is_string {
            return; // null `ends` stays empty
        }
        let f = &mut self.fields;
        let field = match s.key {
            "event" => &mut f.event,
            "severity" => &mut f.severity,
            "status" => &mut f.status,
            "messageType" => &mut f.message_type,
            "ends" => &mut f.ends,
            "expires" => &mut f.expires,
            _ => return,
        };
        s.text.clone_into(field);
    }

    fn object_end(&mut self, key: &str) {
        if key != "properties" {
            return;
        }
        let f = std::mem::take(&mut self.fields);
        if f.status != "Actual" || f.message_type == "Cancel" {
            return;
        }
        let level = alert_level_of(&f.event, &f.severity);
        if level == AlertLevel::None {
            return;
        }
        let ends = parse_iso_time(&f.ends).or_else(|| parse_iso_time(&f.expires));
        self.alerts.push(Alert { level, event: f.event, ends });
    }
}

/// The active alerts: api.weather.gov `/alerts/active?point=...`, GeoJSON.
/// Keeps only actual alerts at a tier (alert_level_of), not cancellations.
#[derive(Default, Debug)]
pub struct AlertsReader {
    json: JsonScanner,
    features: Features,
}

impl AlertsReader {
    pub fn new() -> Self {
        AlertsReader::default()
    }

    pub fn feed(&mut self, chunk: &[u8]) {
        self.json.feed(chunk, &mut self.features);
    }

    pub fn alerts(&self) -> &[Alert] {
        &self.features.alerts
    }

    /// What the lights get: those not ended by `wall_now`, one per event
    /// name (the latest ending), highest tier first, at most Alerts::MAX.
    /// Each one's end becomes a boot-clock time from `boot_now` (the same
    /// moment); one without an end lasts an hour.
    pub fn in_force(&self, wall_now: SysSeconds, boot_now: Instant) -> Alerts {
        // One per event name, the latest ending: an update repeats its alert.
        let mut latest: Vec<(AlertLevel, &str, SysSeconds)> = Vec::new();
        for a in self.alerts() {
            let ends = a.ends.unwrap_or(wall_now + Duration::hours(1));
            if ends <= wall_now {
                continue;
            }
            match latest.iter_mut().find(|l| l.1 == a.event) {
                None => latest.push((a.level, &a.event, ends)),
                Some(same) => same.2 = same.2.max(ends),
            }
        }
        latest.sort_by_key(|a| std::cmp::Reverse(a.0)); // stable: same tiers keep their order

        let mut out = Alerts::default();
        for (level, event, ends) in latest.into_iter().take(Alerts::MAX) {
            let mut alert = ev::Alert { level, until: boot_now + (ends - wall_now), ..ev::Alert::default() };
            alert.set_event(event);
            out.push(alert);
        }
        out
    }
}
