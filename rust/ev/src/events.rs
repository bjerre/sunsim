//! Every event in the system, in one enum. A module never calls another
//! module: it posts one of these, and the bus delivers it to every module
//! attached to it (EventBus::attach), each picking out the ones it handles.
//! See ARCHITECTURE.md §7 for who posts what.

use std::ops::Deref;
use std::sync::Arc;

use crate::installation::{DEFAULT_PEAKS, PerZone};
use crate::time::{Duration, Instant, Transition};
use crate::topology::ZoneId;
use crate::types::*;

// ---- Clock ----------------------------------------------------------------

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct Heartbeat {
    // raw 1 Hz timer; the Clock turns it into Tick
    pub at: Instant, // when it fired
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct WakeAt {
    // a module's Timer wants a tick at `at`, not at the next Tick (Clock)
    pub at: Instant,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct Wake {
    // the Clock, at a WakeAt's time: modules move their Timers on
    pub at: Instant,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct Tick {
    // 1 Hz, with the program's current time
    pub at: Instant,
    pub time: TimeReading,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct ProgramTick {
    // every daylight update interval (60 s, 1 s in fast-day)
    pub time: TimeReading,
    pub transition: Transition, // ramp over the whole interval
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SetFastDay {
    // console `fastday`: the debug fast-day clock on or off
    pub on: bool,
    pub day_length: Duration, // real seconds per simulated active window
}
impl Default for SetFastDay {
    fn default() -> Self {
        SetFastDay { on: false, day_length: Duration::secs(120) }
    }
}

// ---- Settings -------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct ConfigChanged {
    // at boot from NVS, then after every change
    pub config: ProgramConfig,
    pub peaks: Peaks,
    pub min_levels: MinLevels,
}
impl Default for ConfigChanged {
    fn default() -> Self {
        ConfigChanged { config: ProgramConfig::default(), peaks: DEFAULT_PEAKS, min_levels: DEFAULT_MIN_LEVELS }
    }
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct SetConfig {
    // console `config ...`
    pub config: ProgramConfig,
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SetIntensity {
    // console `intensity ...`
    pub lights: Selection, // non-bulbs in it are skipped
    pub percent: u8,
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SetMinLevel {
    // console `minlevel ...`, `tune save`
    pub lights: Selection, // non-bulbs in it are skipped
    pub level: u8,         // 1-254
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct CloudCoverUpdated {
    // InternetSync, after each daily fetch
    pub forecast: CloudCover,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct ConditionsUpdated {
    // InternetSync, every 15 min; console `weather simulate`
    pub now: Conditions,
    pub at: Instant, // when fetched: it goes stale WEATHER_FRESH_FOR later
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct AlertsUpdated {
    // InternetSync, every 15 min (none in force: count 0)
    pub alerts: Alerts,
    pub at: Instant,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct SkyChanged {
    // SkyModel, whenever the sky now changes
    pub sky: Sky,
}
#[derive(Clone, Default, PartialEq, Debug)]
pub struct RolesLoaded {
    // at boot from NVS
    pub records: Arc<DeviceRecords>,
}
#[derive(Clone, Default, PartialEq, Debug)]
pub struct PersistRoles {
    // DeviceRegistry -> SettingsStore
    pub records: Arc<DeviceRecords>,
}

// ---- From the Zigbee stack ------------------------------------------------

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct NetworkUp {
    // network formed, or rejoined after a reboot
    pub rejoined: bool,
    pub at: Instant,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct DeviceAnnounced {
    pub ext_addr: u64,
    pub short_addr: u16,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct EndpointFound {
    // reply to Discover; endpoint 0 = none found
    pub role: Role,
    pub endpoint: u8,
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct BindResult {
    pub role: Role,
    pub ok: bool,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct PingResult {
    pub role: Role,
    pub ok: bool,
    pub error: i32,
    pub status: i32,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct AckReceived {
    // ZCL Default Response from a bulb
    pub short_addr: u16,
    pub rssi: i8,
    pub status: u8,
}

/// What each one does is ModeController's call.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub enum Press {
    #[default]
    Single,
    Double,
    Hold,
    Ambiguous, // On/Toggle seen only as an attribute change: single vs double by timing
}
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub enum Source {
    #[default]
    Zigbee,
    Console,
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct ButtonPressed {
    pub button: Role, // which one
    pub press: Press,
    pub source: Source,
    pub at: Instant,
}
impl Default for ButtonPressed {
    fn default() -> Self {
        ButtonPressed {
            button: Role::new(Group::Button, 0),
            press: Press::Single,
            source: Source::Zigbee,
            at: Instant::ZERO,
        }
    }
}

// ---- Console requests -----------------------------------------------------

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct PairRequested {
    pub role: Role,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct PairCanceled;
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct UnpairRequested {
    pub roles: Selection,
}
/// What the stack takes: whole seconds, up to 254. Pairing, console `open`,
/// network up.
pub const DEFAULT_OPEN_SECS: u8 = 180;
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct OpenNetwork {
    pub secs: u8,
}
impl Default for OpenNetwork {
    fn default() -> Self {
        OpenNetwork { secs: DEFAULT_OPEN_SECS }
    }
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct FactoryReset;
/// Fetch from the internet now (clock, ...): console `sync`, fast-day switches.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct SyncRequested;
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct ModeRequested {
    // console: `tune`, `tune off`
    pub mode: Mode,
    pub zone: Option<ZoneId>, // None: every zone that can run it
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct TuneLights {
    // console `tune <lights> <level> [kelvin]`: the tuning program's levels
    pub lights: Selection,
    pub level: u8,
    pub kelvin: Option<u16>, // none: keep the color
}
/// Console `tune save`: the tuned levels become the minimums.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct TuneSave;

// ---- DeviceRegistry -------------------------------------------------------

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct RoleAssigned {
    // at boot, on pairing, and on any address change
    pub role: Role,
    pub record: DeviceRecord,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct RoleCleared {
    pub role: Role,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct Discover {
    pub role: Role,
    pub short_addr: u16,
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Bind {
    // button's On/Off output -> our receiver endpoint
    pub role: Role,
    pub ext_addr: u64,
    pub short_addr: u16,
    pub endpoint: u8,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct Ping {
    pub role: Role,
    pub short_addr: u16,
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct ReachabilityChanged {
    // a bulb stopped answering / answers again
    pub role: Role,
    pub reachable: bool,
}
#[derive(Clone, Default, PartialEq, Debug)]
pub struct DevicesUpdated {
    // 1 Hz summary for the status model
    pub devices: Arc<PerRole<DeviceStatus>>,
    pub records: Arc<DeviceRecords>,
    pub armed: Option<Role>,
}

// ---- Mode, program, output ------------------------------------------------

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct ModeChanged {
    // a zone switches to another program
    pub zone: ZoneId,
    pub mode: Mode,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct RefreshLights {
    // the running program re-applies now, with its quick transition
    pub role: Option<Role>, // None = every light
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct LightChange {
    // a bulb, what it's to show, and how quickly to get there
    pub role: Role,
    pub state: LightState,
    pub level_transition: Transition,
    pub color_transition: Transition,
}
/// What a bulb should show, from the running program.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct LightTarget(pub LightChange);
/// What it does show: Overlays' pick, for LightOutput; and which weather
/// effect, if any, has the light (none: its program does), in which phase.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct OutputTarget {
    pub change: LightChange,
    pub weather_effect: WeatherEffect,
    pub pulse_phase: PulsePhase,
}

impl From<LightChange> for OutputTarget {
    /// The program's: no weather effect.
    fn from(change: LightChange) -> Self {
        OutputTarget { change, ..OutputTarget::default() }
    }
}

impl Deref for LightTarget {
    type Target = LightChange;
    fn deref(&self) -> &LightChange {
        &self.0
    }
}
impl Deref for OutputTarget {
    type Target = LightChange;
    fn deref(&self) -> &LightChange {
        &self.change
    }
}

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct SetLight {
    // one bulb's ZCL commands, sent in field order (SendQueue)
    pub role: Role,
    pub short_addr: u16,
    pub endpoint: u8,
    pub power: Option<bool>, // On goes first, Off last
    pub level: Option<u8>,
    pub color: Option<Color>,
    pub level_transition: Transition,
    pub color_transition: Transition,
}
/// SendQueue has commands waiting: the ZigbeeAdapter gives it a SendSlot soon.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct SendsWaiting;
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct SendSlot {
    // the ZigbeeAdapter has room for one command now
    pub at: Instant,
}
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Destination {
    // a light's endpoint, or a Zigbee group's lights
    pub address: u16, // a short address, or a group id
    pub endpoint: u8, // unused for a group
    pub group: bool,
}
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct SendLight {
    // one ZCL command (SendQueue -> ZigbeeAdapter)
    pub to: Destination,
    pub power: Option<bool>, // exactly one of these three
    pub level: Option<u8>,
    pub color: Option<Color>,
    pub transition: Transition,
    pub backlog: u8, // lights with commands waiting, this one's included
}

// ---- Status ---------------------------------------------------------------

/// A zone, as the status shows it.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct ZoneStatus {
    pub mode: Mode,
    pub gate_open: bool, // its gate lets its lights be on
}

/// Everything the `status`/`roles`/`config` console commands and the display
/// show, gathered in one place so they always agree.
#[derive(Clone, PartialEq, Debug)]
pub struct StatusSnapshot {
    pub at: Instant, // when this snapshot was taken
    pub time: TimeReading,
    pub day: DayInfo,
    pub zones: PerZone<ZoneStatus>,
    pub devices: PerRole<DeviceStatus>,
    pub records: DeviceRecords,
    pub armed: Option<Role>,
    pub config: ProgramConfig,
    pub peaks: Peaks,
    pub min_levels: MinLevels,
    pub sky: Sky,
}

impl Default for StatusSnapshot {
    fn default() -> Self {
        StatusSnapshot {
            at: Instant::ZERO,
            time: TimeReading::default(),
            day: DayInfo::default(),
            zones: PerZone::default(),
            devices: PerRole::default(),
            records: PerRole::default(),
            armed: None,
            config: ProgramConfig::default(),
            peaks: DEFAULT_PEAKS,
            min_levels: DEFAULT_MIN_LEVELS,
            sky: Sky::default(),
        }
    }
}

#[derive(Clone, Default, PartialEq, Debug)]
pub struct StatusChanged {
    // shared, not copied: it's by far the largest payload, and every Event
    // is as big as its largest
    pub snapshot: Arc<StatusSnapshot>,
}

// ---- The event type ------------------------------------------------------

/// A type that travels on the bus: one of Event's payloads.
pub trait Payload: Into<Event> + Clone {
    /// The payload, if `e` carries one of these.
    fn of(e: &Event) -> Option<&Self>;
}

macro_rules! events {
    ($($name:ident),* $(,)?) => {
        /// One event: a payload, by its type.
        #[derive(Clone, Debug)]
        pub enum Event {
            $($name($name),)*
        }

        impl Event {
            /// Its payload's name, for tracing: "Tick".
            pub fn name(&self) -> &'static str {
                match self {
                    $(Event::$name(_) => stringify!($name),)*
                }
            }
        }

        $(
            impl From<$name> for Event {
                fn from(e: $name) -> Event {
                    Event::$name(e)
                }
            }
            impl Payload for $name {
                fn of(e: &Event) -> Option<&$name> {
                    match e {
                        Event::$name(p) => Some(p),
                        #[allow(unreachable_patterns)]
                        _ => None,
                    }
                }
            }
        )*
    };
}

events! {
    Heartbeat, WakeAt, Wake, Tick, ProgramTick, SetFastDay,
    ConfigChanged, SetConfig, SetIntensity, SetMinLevel, CloudCoverUpdated,
    ConditionsUpdated, AlertsUpdated, SkyChanged,
    RolesLoaded, PersistRoles,
    NetworkUp, DeviceAnnounced, EndpointFound, BindResult, PingResult,
    AckReceived, ButtonPressed,
    PairRequested, PairCanceled, UnpairRequested, OpenNetwork, FactoryReset,
    SyncRequested, ModeRequested, TuneLights, TuneSave,
    RoleAssigned, RoleCleared, Discover, Bind, Ping, ReachabilityChanged,
    DevicesUpdated,
    ModeChanged, RefreshLights, LightTarget, OutputTarget, SetLight,
    SendsWaiting, SendSlot, SendLight,
    StatusChanged,
}

impl Event {
    /// The payload, if it's a T: `e.get::<Tick>()`.
    pub fn get<T: Payload>(&self) -> Option<&T> {
        T::of(self)
    }
}

// Every event is as big as its largest payload, and a minute's burst
// queues a few per light: what grows with the layout is shared (Arc), so
// this holds however many devices there are. (The sky's alerts set it now.)
const _: () = assert!(size_of::<Event>() <= 256, "a payload outgrew Event: share it (Arc<T>)");

/// What every module gets instead of a reference to the bus or to another
/// module. Thread-safe: may be called from any task.
#[derive(Clone)]
pub struct Post(Arc<dyn Fn(Event) + Send + Sync>);

impl Post {
    pub fn new(f: impl Fn(Event) + Send + Sync + 'static) -> Self {
        Post(Arc::new(f))
    }
    pub fn send(&self, e: impl Into<Event>) {
        (self.0)(e.into())
    }
}
