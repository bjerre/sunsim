//! The words the room is described in (installation.rs): which device is
//! which (Address), what a light shows (a Look), when the lights may be on (a
//! Gate), and the zones that tie them together. Plain data, like types.rs;
//! sunsim_core evaluates them.
//!
//! The checks run as the tables are built in a `const`, so a mistake in the
//! room is a compile error: a `panic!` while evaluating a constant stops the
//! build with its message ("a light is in no zone").

use crate::time::Duration;
use crate::types::*;

// ---- Building blocks --------------------------------------------------------

/// A ProgramConfig field, read and written: a setting, which `config <name>`
/// changes and NVS keeps. `field!(active)`.
pub struct Field<T> {
    pub get: fn(&ProgramConfig) -> T,
    pub set: fn(&mut ProgramConfig, T),
}

impl<T> Clone for Field<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for Field<T> {}

/// The ProgramConfig field `name`, as a Field.
#[macro_export]
macro_rules! field {
    ($name:ident) => {
        $crate::topology::Field {
            get: |c: &$crate::types::ProgramConfig| c.$name,
            set: |c: &mut $crate::types::ProgramConfig, v| c.$name = v,
        }
    };
}

/// A value fixed in the installation, or a ProgramConfig field (a setting).
/// `value(config)` reads it.
#[derive(Clone, Copy)]
pub enum Tunable<T: Copy + 'static> {
    Fixed(T),
    Setting(Field<T>),
}

impl<T: Copy> Tunable<T> {
    pub fn value(&self, config: &ProgramConfig) -> T {
        match self {
            Tunable::Fixed(v) => *v,
            Tunable::Setting(f) => (f.get)(config),
        }
    }
}

impl<T: Copy> std::fmt::Debug for Tunable<T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Tunable::Fixed(_) => "Fixed",
            Tunable::Setting(_) => "Setting",
        })
    }
}

/// `field` as a Tunable: `setting!(work)`.
#[macro_export]
macro_rules! setting {
    ($name:ident) => {
        $crate::topology::Tunable::Setting($crate::field!($name))
    };
}

// ---- Addresses: which device is which ---------------------------------------

/// A device's IEEE (EUI-64) address, from its label: when it joins, it takes
/// `role`.
#[derive(Clone, Copy, Debug)]
pub struct Address {
    pub role: &'static str, // one role: "WallA", "Work", any case
    pub ext_addr: u64,      // 0: not known yet (claim it with `pair`)
}

/// Each role's address; 0: none given.
pub type Addresses = PerRole<u64>;

/// `entries` per role: a name that isn't one role, a role listed twice, or
/// one address given to two roles doesn't compile.
pub const fn addresses_of(entries: &[Address]) -> Addresses {
    let mut addresses = PerRole::filled(0u64);
    let mut listed = PerRole::filled(false);
    let mut i = 0;
    while i < entries.len() {
        let entry = entries[i];
        let Some(role) = parse_role(entry.role) else {
            panic!("an address names no role");
        };
        if *listed.get(role) {
            panic!("a role has two addresses");
        }
        if entry.ext_addr != 0 {
            let mut r = 0;
            while r < ROLE_COUNT {
                if addresses.0[r] == entry.ext_addr {
                    panic!("an address is given to two roles");
                }
                r += 1;
            }
        }
        *listed.get_mut(role) = true;
        *addresses.get_mut(role) = entry.ext_addr;
        i += 1;
    }
    addresses
}

// ---- Looks: what a light shows ----------------------------------------------

/// Cool -> warm across the hours, easing in so it stays cool through the
/// morning; cool before them, warm after. Full brightness.
#[derive(Clone, Copy, Debug)]
pub struct WorkCurve {
    pub hours: Tunable<DaySpan>,
    pub cool: Tunable<u16>,
    pub warm: Tunable<u16>,
}

impl WorkCurve {
    /// The work hours and colors, as the settings have them.
    pub const SETTINGS: WorkCurve =
        WorkCurve { hours: setting!(work), cool: setting!(work_cool_mireds), warm: setting!(work_warm_mireds) };
}

/// What a light shows.
#[derive(Clone, Copy, Debug)]
pub enum Look {
    /// The sun at the configured location: warm and dim at sunrise and
    /// sunset, brightest and coolest at solar noon, off at night. A group
    /// showing it is spread along a south wall, east to west in letter order:
    /// the sun direction and the amber glow follow the sun along it. Shaped
    /// by the sun's and the sky's settings (sunshift, amber, clouds, ...).
    SunCurve,
    WorkCurve(WorkCurve),
    /// A binary load has no brightness or color curve; its zone's gate alone
    /// decides when the plug turns on. Plugs only.
    OnOffOnly,
}

/// What a group's lights, or one light, show (in a zone).
#[derive(Clone, Copy, Debug)]
pub struct Shows {
    pub lights: &'static str, // a group ("Wall") or one light ("Work"), any case
    pub look: Look,
    pub peak: u8, // its default brightness, percent of full output (`intensity` changes it)
}

impl Shows {
    /// `lights` show `look`, at full brightness.
    pub const fn new(lights: &'static str, look: Look) -> Self {
        Shows { lights, look, peak: 100 }
    }
    /// At `peak` percent instead.
    pub const fn at(self, peak: u8) -> Self {
        Shows { peak, ..self }
    }
}

// ---- Gates: when the lights may be on ---------------------------------------

/// "On" inside the hours on the days, fading in and out over `fade` at each
/// edge; "off" outside.
#[derive(Clone, Copy, Debug)]
pub struct Window {
    pub hours: Tunable<DaySpan>,
    pub days: Tunable<Weekdays>,
    pub fade: Tunable<Duration>,
}

impl Window {
    /// Every day, no fade.
    pub const fn hours(hours: Tunable<DaySpan>) -> Self {
        Window { hours, days: Tunable::Fixed(EVERY_DAY), fade: Tunable::Fixed(Duration::ZERO) }
    }
    /// Only its "on" (outside, the rules after it decide).
    pub const fn opens_only(self) -> Rule {
        Rule { source: RuleSource::Window(self), says: Says::OnOnly }
    }
    /// Only its "off" (inside, the rules after it decide).
    pub const fn closes_only(self) -> Rule {
        Rule { source: RuleSource::Window(self), says: Says::OffOnly }
    }
    pub const fn rule(self) -> Rule {
        Rule { source: RuleSource::Window(self), says: Says::Both }
    }
}

/// Which of its answers a rule gives; the other case falls to the rules after it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Says {
    Both,
    OnOnly,
    OffOnly,
}

/// Where a rule's answer comes from.
#[derive(Clone, Copy, Debug)]
pub enum RuleSource {
    /// "On", always.
    Always,
    Window(Window),
}

#[derive(Clone, Copy, Debug)]
pub struct Rule {
    pub source: RuleSource,
    pub says: Says,
}

impl Rule {
    pub const ALWAYS: Rule = Rule { source: RuleSource::Always, says: Says::Both };
}

/// A rule that answers whatever the time, so nothing after it in a gate is
/// ever asked: `Always`, and a `Window` that gives both answers. (Presence,
/// once it's here, won't be: it has no answer while its sensor is unknown.)
pub const fn answers_always(rule: &Rule) -> bool {
    matches!(rule.source, RuleSource::Always) || matches!(rule.says, Says::Both)
}

/// The first rule with an answer decides; when none answers, the lights are off.
pub type Gate = &'static [Rule];

// ---- Zones: lights that share a mode, a gate and their buttons --------------

/// A zone, by its place in the zones table.
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash, Debug)]
pub struct ZoneId(pub u8);

/// What a button's presses choose for its zone; None: that press does
/// nothing.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Controls {
    pub by: &'static str, // a button: "Button", any case
    pub single: Option<Mode>,
    pub twice: Option<Mode>, // a double press
    pub hold: Option<Mode>,
}

impl Controls {
    /// Single: daylight, double: red alert, hold: off.
    pub const fn by(button: &'static str) -> Self {
        Controls { by: button, single: Some(Mode::Daylight), twice: Some(Mode::RedAlert), hold: Some(Mode::Off) }
    }
}

/// The lights the weather pulses (sunsim_core::Overlays), each a group or a
/// light: precipitation lights pulse a color while rain or snow falls, alert
/// lights yellow, orange or red for an NWS advisory, watch or warning (an
/// alert wins where they overlap). Color lights in the zone only.
#[derive(Clone, Copy, Debug)]
pub struct Weather {
    pub precip: &'static [&'static str],
    pub alerts: &'static [&'static str],
}

/// Lights that share a mode (chosen by their buttons, or the console) and a
/// gate. Each light is in exactly one zone; it starts in daylight.
#[derive(Clone, Copy, Debug)]
pub struct ZoneSpec {
    pub name: &'static str,
    pub lights: &'static [Shows],
    pub gate: Gate,
    pub modes: &'static [Mode], // what it may run
    pub controls: &'static [Controls],
    pub weather: Option<Weather>, // none: the weather never touches these lights
}

impl ZoneSpec {
    /// What a zone has unless it says otherwise: the gate always open, every
    /// mode, no controls, no weather. `ZoneSpec { name, lights, ..ZoneSpec::DEFAULT }`.
    pub const DEFAULT: ZoneSpec =
        ZoneSpec { name: "", lights: &[], gate: &[Rule::ALWAYS], modes: &MODES, controls: &[], weather: None };

    pub const fn allows(&self, mode: Mode) -> bool {
        let mut i = 0;
        while i < self.modes.len() {
            if self.modes[i] as u8 == mode as u8 {
                return true;
            }
            i += 1;
        }
        false
    }
}

/// Every zone's weather lights.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct WeatherLights {
    pub precip: RoleSet,
    pub alerts: RoleSet,
}

/// The zones table, per role: checked when it's built.
#[derive(Clone, Copy, Debug)]
pub struct Topology {
    pub zone_of: PerRole<Option<ZoneId>>, // a light's zone, or the zone a button controls
    pub look_of: PerRole<Look>,           // lights
    pub peak_of: PerRole<u8>,             // default brightness, percent
    pub controls_of: PerRole<Option<Controls>>, // buttons that control a zone
    pub weather: WeatherLights,
}

const fn same_zone(a: Option<ZoneId>, b: ZoneId) -> bool {
    matches!(a, Some(z) if z.0 == b.0)
}

const fn add_weather(t: &mut Topology, names: &[&str], id: ZoneId, precip: bool) {
    let mut n = 0;
    while n < names.len() {
        let Some(selection) = parse_selection(names[n]) else {
            panic!("a zone's weather lights name something the layout doesn't have");
        };
        let mut r = 0;
        while r < ROLE_COUNT {
            let role = ROLES[r];
            if selects(selection, role) {
                if !same_zone(*t.zone_of.get(role), id) || !caps(role).color {
                    panic!("a weather light isn't a color light in its zone");
                }
                if precip {
                    t.weather.precip.insert(role);
                } else {
                    t.weather.alerts.insert(role);
                }
            }
            r += 1;
        }
        n += 1;
    }
}

/// `zones` per role. Doesn't compile if two zones share a name; a zone has no
/// lights, no gate, a gate rule after one that always answers
/// (answers_always), or can't run daylight; a name in it isn't in the layout,
/// gives a button a look or a light a second one, or an on/off-only look to
/// anything but a plug; a peak isn't 1-100; a light is in no zone; or
/// a button is named twice, isn't a button, or picks a mode its zone can't run;
/// or a weather light isn't a color light in its zone.
pub const fn topology_of(zones: &[ZoneSpec]) -> Topology {
    let mut t = Topology {
        zone_of: PerRole::filled(None),
        look_of: PerRole::filled(Look::SunCurve),
        peak_of: PerRole::filled(100),
        controls_of: PerRole::filled(None),
        weather: WeatherLights { precip: RoleSet::EMPTY, alerts: RoleSet::EMPTY },
    };
    let mut shown = PerRole::filled(false);
    assert!(zones.len() <= 255, "1-255 zones");
    let mut z = 0;
    while z < zones.len() {
        let zone = &zones[z];
        let id = ZoneId(z as u8);
        let mut other = 0;
        while other < zones.len() {
            if other != z && str_eq(zones[other].name, zone.name) {
                panic!("two zones share a name");
            }
            other += 1;
        }
        if zone.lights.is_empty() {
            panic!("a zone has no lights");
        }
        if zone.gate.is_empty() {
            panic!("a zone has no gate");
        }
        let mut g = 0;
        while g + 1 < zone.gate.len() {
            if answers_always(&zone.gate[g]) {
                panic!("a gate rule follows one that always answers: it's never asked");
            }
            g += 1;
        }
        if !zone.allows(Mode::Daylight) {
            panic!("a zone can't run daylight (every zone starts in it)");
        }
        let mut s = 0;
        while s < zone.lights.len() {
            let shows = zone.lights[s];
            if shows.peak < 1 || shows.peak > 100 {
                panic!("a light's peak isn't between 1 and 100 percent");
            }
            let Some(selection) = parse_selection(shows.lights) else {
                panic!("a zone's lights name something the layout doesn't have");
            };
            let mut r = 0;
            while r < ROLE_COUNT {
                let role = ROLES[r];
                if selects(selection, role) {
                    if !is_light(role) {
                        panic!("a zone gives a button a look");
                    }
                    if matches!(shows.look, Look::OnOffOnly) && !matches!(kind_of(role), Kind::Plug) {
                        panic!("an on/off-only look requires a plug");
                    }
                    if *shown.get(role) {
                        panic!("a light is in two zones, or shown twice");
                    }
                    *shown.get_mut(role) = true;
                    *t.look_of.get_mut(role) = shows.look;
                    *t.peak_of.get_mut(role) = shows.peak;
                    *t.zone_of.get_mut(role) = Some(id);
                }
                r += 1;
            }
            s += 1;
        }
        let mut c = 0;
        while c < zone.controls.len() {
            let controls = zone.controls[c];
            let button = match parse_role(controls.by) {
                Some(b) if is_button(b) => b,
                _ => panic!("a zone's controls name something that isn't a button"),
            };
            if t.controls_of.get(button).is_some() {
                panic!("a button controls two zones, or one twice");
            }
            let picks = [controls.single, controls.twice, controls.hold];
            let mut p = 0;
            while p < picks.len() {
                if let Some(mode) = picks[p]
                    && !zone.allows(mode)
                {
                    panic!("a button picks a mode its zone can't run");
                }
                p += 1;
            }
            *t.controls_of.get_mut(button) = Some(controls);
            *t.zone_of.get_mut(button) = Some(id);
            c += 1;
        }
        if let Some(weather) = zone.weather {
            add_weather(&mut t, weather.precip, id, true);
            add_weather(&mut t, weather.alerts, id, false);
        }
        z += 1;
    }
    let mut r = 0;
    while r < ROLE_COUNT {
        if is_light(ROLES[r]) && !shown.0[r] {
            panic!("a light is in no zone");
        }
        r += 1;
    }
    t
}
