//! The virtual office.

use std::cell::{Cell, RefCell};
use std::fmt::Write as _;
use std::rc::Rc;
use std::sync::Arc;

use ev::*;
use platform::presentation::StatusView;
use sim::{Device, Scheduler, SimNetwork};
use sunsim_core::{CoreModules, compute_sun_position, wire_core};

/// How long a change gets to reach the bulbs: SendQueue spends a slot every
/// 25 ms, a few per light.
const SETTLE: Duration = Duration::secs(2);

// Each device's IEEE address, which the installation's addresses give it,
// so each takes its role as it joins.
fn ext_of(role: Role) -> u64 {
    0x00124B0000000000 + slot(role) as u64
}

/// The page's clock: Ticks once a second with the wall-clock time it set
/// (set_clock, set_minute), and Wakes when asked.
struct PageClock {
    post: Post,
    scheduler: Scheduler,
    time: Rc<Cell<TimeReading>>,
}

impl Module for PageClock {
    fn on(&mut self, e: &Event) {
        match e {
            Event::Heartbeat(e) => {
                self.post.send(Tick { at: e.at, time: self.time.get() });
                let next = e.at + Duration::secs(1);
                self.scheduler.at(next, Heartbeat { at: next });
            }
            Event::WakeAt(e) => self.scheduler.at(e.at, Wake { at: e.at }),
            _ => {}
        }
    }
}

/// The office, simulated: the real core, its devices joining a simulated
/// network as on the board (all of them, at noon on a June Tuesday), and
/// the page's controls as events. Time moves only when the page says.
pub struct Office {
    bus: EventBus,
    scheduler: Scheduler,
    _core: CoreModules,
    network: Rc<RefCell<SimNetwork>>,
    time: Rc<Cell<TimeReading>>,
    config: ProgramConfig,
    status: Rc<RefCell<Option<Arc<StatusSnapshot>>>>,
}

impl Default for Office {
    fn default() -> Self {
        Office::new()
    }
}

impl Office {
    pub fn new() -> Office {
        let mut bus = EventBus::new();
        let scheduler = Scheduler::new();
        let mut addresses = PerRole::filled(0);
        for role in ROLES {
            addresses[role] = ext_of(role);
        }
        let core = CoreModules::new(bus.post(), addresses, Vec::new());
        let network = Rc::new(RefCell::new(SimNetwork::new(scheduler.clone())));
        let time = Rc::new(Cell::new(TimeReading {
            local: LocalDays::ymd(2022, 6, 21) + Duration::hours(12),
            utc_offset: Duration::ZERO,
            valid: true,
            simulated: true,
        }));
        let clock = Rc::new(RefCell::new(PageClock {
            post: bus.post().clone(),
            scheduler: scheduler.clone(),
            time: time.clone(),
        }));
        wire_core(&mut bus, &core);
        bus.attach(&clock);
        bus.attach(&network);
        let status = Rc::new(RefCell::new(None));
        let latest = status.clone();
        bus.on::<StatusChanged>(move |e| *latest.borrow_mut() = Some(e.snapshot.clone()));

        let mut office =
            Office { bus, scheduler, _core: core, network, time, config: ProgramConfig::default(), status };
        office.bus.post().send(ConfigChanged { config: office.config, ..ConfigChanged::default() });
        {
            let mut network = office.network.borrow_mut();
            for role in ROLES {
                network.add(Device::new(ext_of(role), kind_of(role)));
            }
            network.start();
        }
        let first = office.scheduler.now() + Duration::secs(1);
        office.scheduler.at(first, Heartbeat { at: first });
        office.run(Duration::secs(5)); // the network forms; the devices join and are found
        office.tick();
        office
    }

    fn run(&mut self, d: Duration) {
        self.scheduler.run_for(&mut self.bus, d);
    }

    fn now(&self) -> Instant {
        self.scheduler.now()
    }

    // The time, now: programs update at once, and the bulbs follow.
    fn tick(&mut self) {
        let time = self.time.get();
        self.bus.post().send(Tick { at: self.now(), time });
        self.bus.post().send(ProgramTick { time, transition: Transition::secs(60) });
        self.run(SETTLE);
    }

    /// `seconds` of virtual time (0-60), the wall clock standing still: what
    /// beats, pulses or waits moves on.
    pub fn advance(&mut self, seconds: i32) {
        self.run(Duration::secs(seconds.clamp(0, 60) as i64));
    }

    /// The same day at `minute` after midnight (0-1439).
    pub fn set_minute(&mut self, minute: i32) {
        let mut t = self.time.get();
        t.local = t.day() + Duration::minutes(minute.clamp(0, 1439) as i64);
        self.time.set(t);
        self.tick();
    }

    /// False, and nothing changes, for a place that isn't one.
    pub fn set_location(&mut self, latitude: f64, longitude: f64) -> bool {
        if !(-90.0..=90.0).contains(&latitude) || !(-180.0..=180.0).contains(&longitude) {
            return false; // NaN too
        }
        self.config.latitude_deg = latitude;
        self.config.longitude_deg = longitude;
        self.bus.post().send(ConfigChanged { config: self.config, ..ConfigChanged::default() });
        self.tick();
        true
    }

    /// The wall clock where the office is; false, and nothing changes, for
    /// a time that isn't one.
    pub fn set_clock(
        &mut self,
        year: i32,
        month: i32,
        day: i32,
        hour: i32,
        minute: i32,
        utc_offset_minutes: i32,
    ) -> bool {
        if month < 1
            || day < 1
            || !is_valid_date(year, month as u32, day as u32)
            || !(0..24).contains(&hour)
            || !(0..60).contains(&minute)
            || !(-14 * 60..=14 * 60).contains(&utc_offset_minutes)
        {
            return false;
        }
        let mut t = self.time.get();
        t.local = LocalDays::ymd(year, month as u32, day as u32) + Duration::minutes(hour as i64 * 60 + minute as i64);
        t.utc_offset = Duration::minutes(utc_offset_minutes as i64);
        self.time.set(t);
        self.tick();
        true
    }

    /// The button: 0 single, 1 double, 2 hold.
    pub fn press(&mut self, kind: i32) {
        let press = match kind {
            1 => Press::Double,
            2 => Press::Hold,
            _ => Press::Single,
        };
        let button = ROLES.into_iter().find(|&r| is_button(r)).unwrap_or(ROLES[0]);
        self.bus.post().send(ButtonPressed { button, press, source: Source::Zigbee, at: self.now() });
        self.tick();
    }

    /// The weather now: what's falling (0 none, 1 drizzle, 2 rain, 3
    /// freezing rain, 4 snow, 5 thunder) and the cloud cover.
    pub fn weather(&mut self, precip: i32, cloud: i32) {
        let precip = match precip {
            1 => Precip::Drizzle,
            2 => Precip::Rain,
            3 => Precip::FreezingRain,
            4 => Precip::Snow,
            5 => Precip::Thunderstorm,
            _ => Precip::None,
        };
        let now = Conditions { cloud_percent: cloud.clamp(0, 100) as u8, precip, intensity: Intensity::Moderate };
        self.bus.post().send(ConditionsUpdated { now, at: self.now() });
        self.tick();
    }

    /// A weather alert for the next hour: 0 none, 1 advisory, 2 watch, 3 warning.
    pub fn alert(&mut self, level: i32) {
        let level = match level {
            1 => AlertLevel::Advisory,
            2 => AlertLevel::Watch,
            3 => AlertLevel::Warning,
            _ => AlertLevel::None,
        };
        let mut alerts = Alerts::default();
        if level != AlertLevel::None {
            let mut alert = Alert { level, until: self.now() + Duration::hours(1), ..Alert::default() };
            alert.set_event("Browser simulation alert");
            alerts.push(alert);
        }
        self.bus.post().send(AlertsUpdated { alerts, at: self.now() });
        self.tick();
    }

    /// Everything the page shows, as its JSON (web/simulator.ts's
    /// SimulatorState).
    pub fn state_json(&self) -> String {
        let status = self.status.borrow().clone().unwrap_or_default();
        let view = StatusView::of(&status);
        let t = self.time.get();
        let (y, m, d) = t.day().civil();
        let sun = compute_sun_position(
            self.config.latitude_deg,
            self.config.longitude_deg,
            t.day_of_year(),
            t.since_midnight().as_hours_f64(),
            t.utc_offset.as_hours_f64(),
        );
        let mut o = String::new();
        let _ = write!(
            o,
            "{{\"location\":{{\"latitude\":{},\"longitude\":{}}},\"utcOffsetMinutes\":{},\"time\":{},\"weekday\":{},\
             \"simDate\":{},\"sun\":{{\"azimuthDeg\":{},\"elevationDeg\":{}}},\"dayLength\":{},\"nextEvent\":{},\
             \"mode\":{},\"modeTone\":{},\"sky\":{},\"skyTone\":{},\"simMinute\":{},\"lights\":[",
            number(self.config.latitude_deg),
            number(self.config.longitude_deg),
            t.utc_offset.whole_minutes(),
            string(&view.time),
            string(&view.weekday),
            string(&format!("{y:04}-{m:02}-{d:02}")),
            number(sun.azimuth_deg),
            number(sun.elevation_deg),
            string(&view.day_length),
            string(&view.next_event),
            string(view.mode),
            string(view.mode_tone.name()),
            string(&view.sky),
            string(view.sky_tone.name()),
            t.since_midnight().whole_minutes(),
        );
        let network = self.network.borrow();
        for (i, role) in lights().enumerate() {
            let bulb = network.bulb(ext_of(role)).copied().unwrap_or_default();
            let _ = write!(
                o,
                "{}{{\"role\":{},\"on\":{},\"level\":{},\"color\":{}}}",
                if i > 0 { "," } else { "" },
                string(role_name(role)),
                bulb.on,
                bulb.to,
                color(bulb.color.unwrap_or_default())
            );
        }
        o.push_str("],\"devices\":[");
        for (i, role) in ROLES.into_iter().enumerate() {
            let link = match status.devices[role].link {
                DeviceLink::Unpaired => "unpaired",
                DeviceLink::Silent => "silent",
                DeviceLink::Online => "online",
            };
            let _ = write!(
                o,
                "{}{{\"role\":{},\"link\":{},\"endpoint\":{}}}",
                if i > 0 { "," } else { "" },
                string(role_name(role)),
                string(link),
                status.devices[role].has_endpoint
            );
        }
        o.push_str("]}");
        o
    }
}

// A JSON string.
fn string(text: &str) -> String {
    let mut out = String::from("\"");
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

// A JSON number: finite, or null.
fn number(v: f64) -> String {
    if v.is_finite() { v.to_string() } else { "null".into() }
}

fn color(c: Color) -> String {
    match c {
        Color::Mireds(m) => format!("{{\"mode\":\"mireds\",\"value\":{}}}", m.0),
        Color::Xy(xy) => format!("{{\"mode\":\"xy\",\"x\":{},\"y\":{}}}", xy.x, xy.y),
        Color::HueSat(h) => format!("{{\"mode\":\"hue\",\"hue\":{},\"saturation\":{}}}", h.hue, h.saturation),
    }
}
