//! Value types shared by every layer. Plain data only: no behavior beyond
//! trivial helpers.

use std::ops::{Index, IndexMut};

use crate::time::{Duration, Instant, LocalDays, LocalSeconds, Weekday};

/// A stretch of the day, from `start` to `end` after local midnight
/// (start < end <= 24 h): the active hours, the work hours, the sync window.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DaySpan {
    pub start: Duration,
    pub end: Duration,
}

impl DaySpan {
    pub const fn new(start: Duration, end: Duration) -> Self {
        DaySpan { start, end }
    }
    pub fn contains(self, since_midnight: Duration) -> bool {
        since_midnight >= self.start && since_midnight < self.end
    }
    pub fn valid(self) -> bool {
        self.start >= Duration::ZERO && self.end <= Duration::days(1) && self.start < self.end
    }
}

/// A set of weekdays.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Weekdays {
    pub bits: u8, // bit n: weekday n, as Weekday's encoding (0 = Sunday)
}

impl Weekdays {
    pub const fn contains(self, day: Weekday) -> bool {
        (self.bits >> day.0) & 1 != 0
    }
    pub fn insert(&mut self, day: Weekday) {
        self.bits |= 1 << day.0;
    }
}
pub const MONDAY_TO_FRIDAY: Weekdays = Weekdays { bits: 0b0111110 };
pub const EVERY_DAY: Weekdays = Weekdays { bits: 0b1111111 };

// ---- Roles ----------------------------------------------------------------

/// What a device does. Each group can hold any number of devices.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Group {
    Wall,
    Work,
    Button,
}

/// What a device is, and so what it can do (caps()).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Kind {
    ColorBulb, // on/off, level, color (temperature, xy and hue/saturation)
    Plug,      // on/off, for whatever's plugged into it
    Button,    // presses: its On/Off client, bound to us
}

/// What a device takes from us. A light takes at least On/Off; a button
/// takes nothing (it sends).
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Caps {
    pub power: bool, // On/Off
    pub level: bool, // Level Control
    pub color: bool, // Color Control
}

impl Kind {
    pub const fn caps(self) -> Caps {
        match self {
            Kind::ColorBulb => Caps { power: true, level: true, color: true },
            Kind::Plug => Caps { power: true, level: false, color: false },
            Kind::Button => Caps { power: false, level: false, color: false },
        }
    }
}

/// N devices of one kind with one job.
#[derive(Clone, Copy, Debug)]
pub struct GroupSpec {
    pub group: Group,
    pub name: &'static str,
    pub kind: Kind,
    pub count: u8,
}

/// The installation: what each group is and how many it has, in Group
/// order. A group's members are lettered (WallA, WallB, ...); a group of one
/// is just its name (Work, Button). Wall lights are lettered east to west:
/// their order along the wall drives the sun-direction dimming.
pub const LAYOUT: [GroupSpec; 3] = [
    GroupSpec { group: Group::Wall, name: "Wall", kind: Kind::ColorBulb, count: 5 },
    GroupSpec { group: Group::Work, name: "Work", kind: Kind::ColorBulb, count: 1 },
    GroupSpec { group: Group::Button, name: "Button", kind: Kind::Button, count: 1 },
];

/// One device's job: its group, and which member of it. Assigned at pairing
/// time (console `pair`, or its address in installation.rs), never by join
/// order.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Role {
    pub group: Group,
    pub index: u8, // within the group; for the wall, 0 = the east end
}

impl Role {
    pub const fn new(group: Group, index: u8) -> Self {
        Role { group, index }
    }
}

impl Default for Role {
    fn default() -> Self {
        Role::new(Group::Wall, 0)
    }
}

pub const fn spec(group: Group) -> &'static GroupSpec {
    &LAYOUT[group as usize]
}

pub const ROLE_COUNT: usize = {
    let mut n = 0;
    let mut g = 0;
    while g < LAYOUT.len() {
        n += LAYOUT[g].count as usize;
        g += 1;
    }
    n
};

/// Where a role's data lives in a PerRole: groups in Group order, members in
/// order within each.
pub const fn slot(role: Role) -> usize {
    let mut s = role.index as usize;
    let mut g = 0;
    while g < role.group as usize {
        s += LAYOUT[g].count as usize;
        g += 1;
    }
    s
}

/// Every role, in slot order.
pub const ROLES: [Role; ROLE_COUNT] = {
    let mut roles = [Role::new(Group::Wall, 0); ROLE_COUNT];
    let mut g = 0;
    while g < LAYOUT.len() {
        let mut i = 0;
        while i < LAYOUT[g].count {
            let role = Role::new(LAYOUT[g].group, i);
            roles[slot(role)] = role;
            i += 1;
        }
        g += 1;
    }
    roles
};

/// A group's roles, in order (for the wall: east to west).
pub fn members(group: Group) -> &'static [Role] {
    let first = slot(Role::new(group, 0));
    &ROLES[first..first + spec(group).count as usize]
}

pub const fn kind_of(role: Role) -> Kind {
    spec(role.group).kind
}
pub const fn caps(role: Role) -> Caps {
    kind_of(role).caps()
}

/// A light: something we switch (a bulb or a plug), not a button.
pub const fn is_light(role: Role) -> bool {
    caps(role).power
}
pub const fn is_button(role: Role) -> bool {
    matches!(kind_of(role), Kind::Button)
}

/// The lights (every role but the buttons), in slot order.
pub fn lights() -> impl Iterator<Item = Role> {
    ROLES.into_iter().filter(|&r| is_light(r))
}
pub const LIGHT_COUNT: usize = {
    let mut n = 0;
    let mut i = 0;
    while i < ROLE_COUNT {
        if is_light(ROLES[i]) {
            n += 1;
        }
        i += 1;
    }
    n
};

/// One T per role, indexed by the role.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct PerRole<T>(pub [T; ROLE_COUNT]);

impl<T: Copy> PerRole<T> {
    pub const fn filled(value: T) -> Self {
        PerRole([value; ROLE_COUNT])
    }
}

impl<T: Default> Default for PerRole<T> {
    fn default() -> Self {
        PerRole(std::array::from_fn(|_| T::default()))
    }
}

impl<T> PerRole<T> {
    pub const fn get(&self, role: Role) -> &T {
        &self.0[slot(role)]
    }
    pub const fn get_mut(&mut self, role: Role) -> &mut T {
        &mut self.0[slot(role)]
    }
}

impl<T> Index<Role> for PerRole<T> {
    type Output = T;
    fn index(&self, role: Role) -> &T {
        &self.0[slot(role)]
    }
}
impl<T> IndexMut<Role> for PerRole<T> {
    fn index_mut(&mut self, role: Role) -> &mut T {
        &mut self.0[slot(role)]
    }
}

const _: () = {
    let mut g = 0;
    while g < LAYOUT.len() {
        assert!(LAYOUT[g].count >= 1 && LAYOUT[g].count <= 26, "a group has 1-26 members (lettered A-Z)");
        assert!(LAYOUT[g].group as usize == g, "LAYOUT must list the groups in Group order");
        g += 1;
    }
    let mut buttons = 0;
    let mut i = 0;
    while i < ROLE_COUNT {
        if is_button(ROLES[i]) {
            buttons += 1;
        }
        i += 1;
    }
    assert!(buttons <= 1, "one button at most: ZigbeeAdapter tells presses apart by its one address");
    assert!(ROLE_COUNT <= 64, "a RoleSet holds 64 roles");
};

// ---- Names ----------------------------------------------------------------

const fn lower(c: u8) -> u8 {
    c.to_ascii_lowercase()
}

/// `a` and `b` alike, ignoring ASCII case.
pub const fn equals_ignoring_case(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    let mut i = 0;
    while i < a.len() {
        if lower(a[i]) != lower(b[i]) {
            return false;
        }
        i += 1;
    }
    true
}

/// `a` and `b` exactly alike: `==` for a const fn.
pub const fn str_eq(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    let mut i = 0;
    while i < a.len() {
        if a[i] != b[i] {
            return false;
        }
        i += 1;
    }
    true
}

/// Whether `name` is `role`'s name ("WallA", "Work"), ignoring case.
const fn is_named(role: Role, name: &str) -> bool {
    let g = spec(role.group);
    let (gn, n) = (g.name.as_bytes(), name.as_bytes());
    let lettered = g.count > 1;
    if n.len() != gn.len() + lettered as usize {
        return false;
    }
    let mut i = 0;
    while i < gn.len() {
        if lower(gn[i]) != lower(n[i]) {
            return false;
        }
        i += 1;
    }
    !lettered || lower(n[gn.len()]) == lower(b'A' + role.index)
}

struct RoleName {
    chars: [u8; 12],
    len: usize,
}

static ROLE_NAMES: [RoleName; ROLE_COUNT] = {
    let mut names = [const { RoleName { chars: [0; 12], len: 0 } }; ROLE_COUNT];
    let mut i = 0;
    while i < ROLE_COUNT {
        let role = ROLES[i];
        let g = spec(role.group);
        let name = g.name.as_bytes();
        assert!(name.len() < 12, "a group's name fits a role name");
        let mut c = 0;
        while c < name.len() {
            names[i].chars[c] = name[c];
            c += 1;
        }
        names[i].len = name.len();
        if g.count > 1 {
            names[i].chars[c] = b'A' + role.index;
            names[i].len += 1;
        }
        i += 1;
    }
    names
};

/// "WallA" ... "WallE", "Work", "Button".
pub fn role_name(role: Role) -> &'static str {
    let n = &ROLE_NAMES[slot(role)];
    std::str::from_utf8(&n.chars[..n.len]).expect("role names are ASCII")
}

/// A role by name, ignoring case ("WallA", "walla").
pub const fn parse_role(name: &str) -> Option<Role> {
    let mut i = 0;
    while i < ROLE_COUNT {
        if is_named(ROLES[i], name) {
            return Some(ROLES[i]);
        }
        i += 1;
    }
    None
}

/// Which roles a request is about: every role, one group, or one role.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Selection {
    #[default]
    AllRoles,
    Group(Group),
    Role(Role),
}

impl From<Group> for Selection {
    fn from(g: Group) -> Self {
        Selection::Group(g)
    }
}
impl From<Role> for Selection {
    fn from(r: Role) -> Self {
        Selection::Role(r)
    }
}

pub const fn selects(selection: Selection, role: Role) -> bool {
    match selection {
        Selection::AllRoles => true,
        Selection::Group(g) => role.group as u8 == g as u8,
        Selection::Role(r) => r.group as u8 == role.group as u8 && r.index == role.index,
    }
}

/// "all", a role ("WallC") or a group ("wall"), ignoring case. A group of
/// one is its role ("Work").
pub const fn parse_selection(text: &str) -> Option<Selection> {
    if equals_ignoring_case(text, "all") {
        return Some(Selection::AllRoles);
    }
    if let Some(role) = parse_role(text) {
        return Some(Selection::Role(role));
    }
    let mut g = 0;
    while g < LAYOUT.len() {
        if equals_ignoring_case(LAYOUT[g].name, text) {
            return Some(Selection::Group(LAYOUT[g].group));
        }
        g += 1;
    }
    None
}

/// Any number of roles, by slot: the weather lights.
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash, Debug)]
pub struct RoleSet(u64);

impl RoleSet {
    pub const EMPTY: RoleSet = RoleSet(0);

    pub const fn contains(self, role: Role) -> bool {
        self.0 >> slot(role) & 1 != 0
    }
    pub const fn insert(&mut self, role: Role) {
        self.0 |= 1 << slot(role);
    }
    pub const fn with(mut self, role: Role) -> Self {
        self.insert(role);
        self
    }
    pub const fn len(self) -> usize {
        self.0.count_ones() as usize
    }
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
    /// Every role in `self` is in `other`.
    pub const fn is_subset(self, other: RoleSet) -> bool {
        self.0 & !other.0 == 0
    }
}

impl FromIterator<Role> for RoleSet {
    fn from_iter<I: IntoIterator<Item = Role>>(roles: I) -> Self {
        roles.into_iter().fold(RoleSet::EMPTY, RoleSet::with)
    }
}

/// A paired device's addresses. short_addr is network-assigned and refreshed
/// on every announce; endpoint is 0 until discovery finds it.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DeviceRecord {
    pub valid: bool,
    pub ext_addr: u64,
    pub short_addr: u16,
    pub endpoint: u8,
}
pub type DeviceRecords = PerRole<DeviceRecord>;

// ---- Light state ----------------------------------------------------------

/// A white on the blackbody line, by color temperature: mireds = 1e6 / kelvin.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Mireds(pub u16);

/// A CIE 1931 chromaticity: warmer than color-temperature mode can show
/// (amber, below most bulbs' ~2700 K).
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct Xy {
    pub x: f32,
    pub y: f32,
}

/// A saturated color: Zigbee's hue (0-254 for 0-360 deg) and saturation (0-254).
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct HueSat {
    pub hue: u8,
    pub saturation: u8,
}

/// A light's color, in whichever of the bulbs' three color modes shows it.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Color {
    Mireds(Mireds),
    Xy(Xy),
    HueSat(HueSat),
}

impl Default for Color {
    fn default() -> Self {
        Color::Mireds(Mireds::default())
    }
}
impl From<Mireds> for Color {
    fn from(c: Mireds) -> Self {
        Color::Mireds(c)
    }
}
impl From<Xy> for Color {
    fn from(c: Xy) -> Self {
        Color::Xy(c)
    }
}
impl From<HueSat> for Color {
    fn from(c: HueSat) -> Self {
        Color::HueSat(c)
    }
}

impl Color {
    pub fn mireds(self) -> Option<Mireds> {
        if let Color::Mireds(c) = self { Some(c) } else { None }
    }
    pub fn xy(self) -> Option<Xy> {
        if let Color::Xy(c) = self { Some(c) } else { None }
    }
    pub fn hue_sat(self) -> Option<HueSat> {
        if let Color::HueSat(c) = self { Some(c) } else { None }
    }
}

pub const MAX_LEVEL: u8 = 254; // Zigbee's brightest

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct LightState {
    pub on: bool,
    pub level: u8, // 0-254; this and the color are meaningful only if `on`
    pub color: Color,
}

// ---- Modes and links ------------------------------------------------------

/// Which lighting program is running (sunsim_core::ProgramHost runs it).
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash, Debug)]
pub enum Mode {
    #[default]
    Daylight,
    Off,
    RedAlert,
    Tuning,
}
pub const MODES: [Mode; 4] = [Mode::Daylight, Mode::Off, Mode::RedAlert, Mode::Tuning];

impl Mode {
    pub const fn name(self) -> &'static str {
        match self {
            Mode::Daylight => "daylight",
            Mode::Off => "off",
            Mode::RedAlert => "RED ALERT",
            Mode::Tuning => "tuning",
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub enum DeviceLink {
    #[default]
    Unpaired, // no device holds this role
    Silent, // paired, but not heard from recently (or never, for a button)
    Online, // heard from recently
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct DeviceStatus {
    pub link: DeviceLink, // what status shows, from the registry's liveness
    pub paired: bool,
    pub has_endpoint: bool,
    pub last_heard: Option<Instant>, // none: not since boot
    pub rssi: i8,                    // of the last ack, if any
    pub reachable: bool,             // false: not answering, light commands paused
}

impl Default for DeviceStatus {
    fn default() -> Self {
        DeviceStatus {
            link: DeviceLink::Unpaired,
            paired: false,
            has_endpoint: false,
            last_heard: None,
            rssi: 0,
            reachable: true,
        }
    }
}

// ---- Program settings -----------------------------------------------------

/// Everything the programs (and the internet sync) can be tuned with. See
/// DESIGN.md §4.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct ProgramConfig {
    pub latitude_deg: f64,  // degrees North (negative = South); 0 = equator
    pub longitude_deg: f64, // degrees East (negative = West); 0 = Greenwich
    pub warm_mireds: u16,   // ~2700K
    pub cool_mireds: u16,   // ~6500K

    /// Sun direction along the wall: the wall bulbs sit east-to-west on a
    /// south wall; the one nearest the sun's east-west position gets the full
    /// curve, those further along dim by up to this much (0 = off).
    pub sun_shift_percent: u8,

    /// Amber sunrise/sunset: below golden_elevation_deg of sun elevation the
    /// wall color slides along the blackbody line down to amber_kelvin at the
    /// horizon (beyond most bulbs' ~2700K color-temperature floor, so xy).
    /// The glow follows the sun along the wall: the bulb nearest it turns
    /// amber first, and the far end lags as if the sun were up to
    /// amber_sweep_deg higher (sunset: the west end deepest; 0 = all alike).
    pub amber_enabled: bool,
    pub golden_elevation_deg: f64,
    pub amber_kelvin: f64,
    pub amber_sweep_deg: u8, // 0-MAX_AMBER_SWEEP_DEG

    /// Active hours: the office's gate (ZONES) lets its lights be on only on
    /// active_days, in the active hours; fading in and out over `fade` at
    /// each edge.
    pub active: DaySpan,
    pub fade: Duration,
    pub active_days: Weekdays,

    /// Work light (WorkCurve): cool -> warm (ease-in) across the core work
    /// hours, cool before them and warm after.
    pub work: DaySpan,
    pub work_cool_mireds: u16, // ~6000K
    pub work_warm_mireds: u16, // ~3000K

    /// Internet sync window: once a day in it, the clock is synced (and the
    /// day's internet data fetched), retrying every 10 min within it on
    /// failure. Before the active window, so the data is ready for the day.
    pub sync: DaySpan,

    /// Cloud cover over the wall (the hourly forecast, CloudCover): under
    /// full overcast the wall is up to cloud_dim_percent dimmer and its
    /// color up to cloud_cool_percent of the way to cool_mireds, and the
    /// sun-direction dimming and amber glow fade out (diffuse light).
    pub clouds_enabled: bool,
    pub cloud_dim_percent: u8,
    pub cloud_cool_percent: u8,

    /// The weather lights (sunsim_core::Overlays; which they are, each zone
    /// declares: Weather). Off: every light shows its program alone.
    pub weather_enabled: bool,
}

impl Default for ProgramConfig {
    fn default() -> Self {
        ProgramConfig {
            latitude_deg: 0.0,
            longitude_deg: 0.0,
            warm_mireds: 370,
            cool_mireds: 153,
            sun_shift_percent: 25,
            amber_enabled: true,
            golden_elevation_deg: 6.0,
            amber_kelvin: 1800.0,
            amber_sweep_deg: 3,
            active: DaySpan::new(Duration::hours(8), Duration::hours(19)),
            fade: Duration::minutes(10),
            active_days: MONDAY_TO_FRIDAY,
            work: DaySpan::new(Duration::minutes(8 * 60 + 30), Duration::minutes(17 * 60 + 30)),
            work_cool_mireds: 167,
            work_warm_mireds: 333,
            sync: DaySpan::new(Duration::hours(4), Duration::hours(5)),
            clouds_enabled: true,
            cloud_dim_percent: 40,
            cloud_cool_percent: 50,
            weather_enabled: true,
        }
    }
}

pub const MAX_AMBER_SWEEP_DEG: u8 = 30;

/// Per-light peak intensity, 1-100 (% of full brightness). Buttons unused.
pub type Peaks = PerRole<u8>;
pub const DEFAULT_PEAKS: Peaks = {
    let mut peaks = PerRole::filled(100);
    let mut i = 0;
    while i < ROLE_COUNT {
        if matches!(ROLES[i].group, Group::Work) {
            peaks.0[i] = 90;
        }
        i += 1;
    }
    peaks
};

/// Per-light minimum level, 1-254 (raw Zigbee level): the lowest a light is
/// sent while on, so it stays visibly lit down to sunset. Bulbs differ in
/// where they go dark; find it with the console's `tune`.
pub type MinLevels = PerRole<u8>;
pub const DEFAULT_MIN_LEVELS: MinLevels = PerRole::filled(1);

// ---- Weather --------------------------------------------------------------

/// The hourly cloud cover forecast (InternetSync fetches it each sync): 48
/// local hours from 00:00 of the day it was fetched.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct CloudCover {
    pub day: Option<LocalDays>, // hour 0's date; none: no forecast yet
    pub percent: [u8; CloudCover::HOURS],
}

impl CloudCover {
    pub const NO_DATA: u8 = 0xFF;
    pub const HOURS: usize = 48;
}

impl Default for CloudCover {
    fn default() -> Self {
        CloudCover { day: None, percent: [CloudCover::NO_DATA; CloudCover::HOURS] }
    }
}

/// What's falling now, from the WMO weather code, and how hard.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub enum Precip {
    #[default]
    None,
    Drizzle,
    Rain,
    FreezingRain,
    Snow,
    Thunderstorm,
}
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub enum Intensity {
    #[default]
    Light,
    Moderate,
    Heavy,
}

impl Precip {
    pub const fn name(self) -> &'static str {
        match self {
            Precip::None => "none",
            Precip::Drizzle => "drizzle",
            Precip::Rain => "rain",
            Precip::FreezingRain => "freezing rain",
            Precip::Snow => "snow",
            Precip::Thunderstorm => "thunderstorm",
        }
    }
}

impl Intensity {
    pub const fn name(self) -> &'static str {
        match self {
            Intensity::Light => "light",
            Intensity::Moderate => "moderate",
            Intensity::Heavy => "heavy",
        }
    }
}

/// The weather now (Open-Meteo's `current`: 15-minute model data, the best
/// estimate of now rather than an observation).
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Conditions {
    pub cloud_percent: u8,
    pub precip: Precip,
    pub intensity: Intensity,
}

/// NWS's alert tiers, as its event names give them: "Winter Storm Warning".
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum AlertLevel {
    #[default]
    None,
    Advisory,
    Watch,
    Warning,
}

impl AlertLevel {
    pub const fn name(self) -> &'static str {
        match self {
            AlertLevel::None => "none",
            AlertLevel::Advisory => "advisory",
            AlertLevel::Watch => "watch",
            AlertLevel::Warning => "warning",
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Alert {
    pub level: AlertLevel,
    pub event: [u8; 31], // "Winter Storm Warning", NUL-padded, cut to fit
    pub until: Instant,  // when it ends
}

impl Alert {
    /// `name` as the event, cut to fit (on a character boundary).
    pub fn set_event(&mut self, name: &str) {
        let mut end = name.len().min(self.event.len());
        while !name.is_char_boundary(end) {
            end -= 1;
        }
        self.event = [0; 31];
        self.event[..end].copy_from_slice(&name.as_bytes()[..end]);
    }
    pub fn event(&self) -> &str {
        let len = self.event.iter().position(|&c| c == 0).unwrap_or(self.event.len());
        std::str::from_utf8(&self.event[..len]).unwrap_or("")
    }
}

/// The NWS alerts for the location, highest level first.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Alerts {
    pub items: [Alert; Alerts::MAX],
    pub count: u8,
}

impl Alerts {
    pub const MAX: usize = 4;

    pub fn list(&self) -> &[Alert] {
        &self.items[..self.count as usize]
    }
    /// Adds `alert` if there's room.
    pub fn push(&mut self, alert: Alert) {
        if (self.count as usize) < Alerts::MAX {
            self.items[self.count as usize] = alert;
            self.count += 1;
        }
    }
}

/// Live weather older than this (three missed 15-minute fetches) is ignored:
/// cloud cover falls back to the forecast, the weather lights to their program.
pub const WEATHER_FRESH_FOR: Duration = Duration::minutes(45);

/// The sky now, as sunsim_core::SkyModel concludes it from the forecast and
/// the live weather: what the daylight program, the weather lights and the
/// status show.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Sky {
    pub cloud_percent: Option<u8>,      // none: no data at all (clear assumed)
    pub cloud_live: bool,               // from the live conditions, not the forecast
    pub conditions: Option<Conditions>, // live, while fresh
    pub alerts: Alerts,                 // in force, highest level first
}

impl Sky {
    pub fn alert(&self) -> AlertLevel {
        if self.alerts.count > 0 { self.alerts.items[0].level } else { AlertLevel::None }
    }
}

// ---- Time -----------------------------------------------------------------

/// What time the program runs on: the real clock, or the debug fast-day clock.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TimeReading {
    pub local: LocalSeconds,  // the wall-clock time where the lights are
    pub utc_offset: Duration, // local - UTC, DST included (+ = East)
    pub valid: bool,          // false until the clock has been set (SNTP)
    pub simulated: bool,      // debug fast-day clock
}

impl TimeReading {
    pub fn day(&self) -> LocalDays {
        self.local.day()
    }
    pub fn since_midnight(&self) -> Duration {
        self.local - self.day()
    }
    pub fn weekday(&self) -> Weekday {
        self.day().weekday()
    }
    /// 1-366.
    pub fn day_of_year(&self) -> i32 {
        let (y, _, _) = self.day().civil();
        (self.day().0 - LocalDays::ymd(y, 1, 1).0) as i32 + 1
    }
}

/// Today's daylight as shown by the status display/console: times are after
/// local midnight, in hours.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct DayInfo {
    pub sunrise: f64,
    pub sunset: f64,
    pub day_length: f64,
    pub next_is_sunrise: bool, // false = next event is today's sunset
    pub next_event: f64,       // when the next event is
    pub until_next: f64,       // how long until it
}
