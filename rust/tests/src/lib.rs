//! What every host test uses: the core, attached like the firmware's, with a
//! log of everything posted; and a fixed day to run it on. Each file in
//! tests/ is an area (devices, modes, daylight, weather, events, sends,
//! sim), as in test/host; `cargo test` runs them all, `cargo test <name>`
//! the ones whose names match.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;
use std::sync::Arc;

use ev::*;
use sunsim_core::{CoreModules, wire_core};

pub const WALL_A: Role = Role::new(Group::Wall, 0); // the east end
pub const WALL_B: Role = Role::new(Group::Wall, 1);
pub const WALL_C: Role = Role::new(Group::Wall, 2);
pub const WALL_D: Role = Role::new(Group::Wall, 3);
pub const WALL_E: Role = Role::new(Group::Wall, 4); // the west end
pub const WORK: Role = Role::new(Group::Work, 0);
pub const DESK: Role = Role::new(Group::Desk, 0); // a plug, on a schedule
pub const GROW: Role = Role::new(Group::Grow, 0); // likewise
pub const BUTTON: Role = Role::new(Group::Button, 0);

pub const fn secs(n: i64) -> Duration {
    Duration::secs(n)
}
pub const fn mins(n: i64) -> Duration {
    Duration::minutes(n)
}
pub const fn hours(n: i64) -> Duration {
    Duration::hours(n)
}
pub const fn ms(n: i64) -> Duration {
    Duration::millis(n)
}

/// noon()'s day: a Tuesday, the 172nd day of the year.
pub const TUESDAY: LocalDays = LocalDays::ymd(2022, 6, 21);

/// That Tuesday at `time` after midnight, clock set, five hours behind UTC.
pub fn at(time: Duration) -> TimeReading {
    TimeReading { local: TUESDAY + time, utc_offset: -hours(5), valid: true, simulated: false }
}

pub fn noon() -> TimeReading {
    at(hours(12))
}

/// The core, wired like the firmware, plus a log of every event posted.
pub struct Harness {
    pub bus: EventBus,
    pub core: CoreModules,
    log: Rc<RefCell<Vec<Event>>>,
    pub now: Instant,
}

impl Default for Harness {
    fn default() -> Self {
        Harness::new()
    }
}

impl Harness {
    pub fn new() -> Self {
        Harness::with_addresses(PerRole::filled(0))
    }

    pub fn with_addresses(addresses: Addresses) -> Self {
        Harness::with_installation(addresses, Installation::built_in())
    }

    /// The core with `installation`'s zones: a profile's.
    pub fn with_installation(addresses: Addresses, installation: Installation) -> Self {
        let mut bus = EventBus::new();
        let core = CoreModules::new(bus.post(), addresses, Vec::new(), Arc::new(installation));
        wire_core(&mut bus, &core);
        // Record everything posted, including platform-bound events.
        let log = Rc::new(RefCell::new(Vec::new()));
        let recorder = log.clone();
        bus.on_any(move |e| recorder.borrow_mut().push(e.clone()));
        let mut h = Harness { bus, core, log, now: Instant::ZERO };
        h.post(NetworkUp::default()); // at time 0, so "15 s after the network is up" is 15 s in
        h.clear_log();
        h
    }

    pub fn post(&mut self, e: impl Into<Event>) {
        self.bus.post().send(e);
        self.bus.drain();
    }

    /// `seconds` Ticks, a second apart, at noon.
    pub fn tick(&mut self, seconds: u32) {
        self.tick_at(seconds, noon());
    }

    pub fn tick_at(&mut self, seconds: u32, time: TimeReading) {
        for _ in 0..seconds {
            self.now += secs(1);
            self.post(Tick { at: self.now, time });
        }
    }

    /// Every T posted since the log was last cleared.
    pub fn all<T: Payload>(&self) -> Vec<T> {
        self.log.borrow().iter().filter_map(T::of).cloned().collect()
    }

    pub fn clear_log(&mut self) {
        self.log.borrow_mut().clear();
    }

    /// Pair `role` through the console flow and complete discovery.
    pub fn pair_bulb(&mut self, role: Role, ext: u64, short_addr: u16) {
        self.pair_bulb_at(role, ext, short_addr, 1);
    }

    pub fn pair_bulb_at(&mut self, role: Role, ext: u64, short_addr: u16, endpoint: u8) {
        self.post(PairRequested { role });
        self.post(DeviceAnnounced { ext_addr: ext, short_addr });
        self.post(EndpointFound { role, endpoint });
    }
}

/// A forecast for noon()'s date: 20% at 12:00, 60% at 13:00, the next day's
/// noon 90%.
pub fn tuesday_forecast() -> CloudCover {
    let mut f = CloudCover { day: Some(TUESDAY), ..CloudCover::default() };
    f.percent[12] = 20;
    f.percent[13] = 60;
    f.percent[36] = 90;
    f
}

/// Its color, if it's in Hue/Saturation mode.
pub fn hue_sat(color: Option<Color>) -> Option<HueSat> {
    color.and_then(Color::hue_sat)
}
pub fn mireds(color: Option<Color>) -> Option<Mireds> {
    color.and_then(Color::mireds)
}
pub fn xy(color: Option<Color>) -> Option<Xy> {
    color.and_then(Color::xy)
}

// ---- NVS, in memory -------------------------------------------------------

/// A stored value, at its width.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Value {
    U8(u8),
    U16(u16),
    U64(u64),
    Str(String),
}

/// NVS in memory: typed, so reading at another width than was written finds
/// nothing, as on the board; keys at most 15 characters. Commits are
/// counted, and writes can be made to fail.
#[derive(Clone, Default, Debug)]
pub struct Nvs {
    pub keys: BTreeMap<String, Value>,
    pub commits: u32,
    pub full: bool, // every write fails, as when NVS is out of space
}

impl Nvs {
    pub fn with(mut self, key: &str, value: Value) -> Self {
        self.keys.insert(key.to_string(), value);
        self
    }

    fn set(&mut self, key: &str, value: Value) -> Result<(), &'static str> {
        assert!(key.len() <= 15, "NVS keys are at most 15 characters: {key}");
        if self.full {
            return Err("ESP_ERR_NVS_NOT_ENOUGH_SPACE");
        }
        self.keys.insert(key.to_string(), value);
        Ok(())
    }
}

macro_rules! typed {
    ($get:ident, $set:ident, $variant:ident, $ty:ty) => {
        fn $get(&self, key: &str) -> Result<Option<$ty>, Self::Error> {
            Ok(match self.keys.get(key) {
                Some(Value::$variant(v)) => Some(v.clone()),
                _ => None,
            })
        }
        fn $set(&mut self, key: &str, value: $ty) -> Result<(), Self::Error> {
            self.set(key, Value::$variant(value))
        }
    };
}

impl platform::Storage for Nvs {
    type Error = &'static str;

    typed!(get_u8, set_u8, U8, u8);
    typed!(get_u16, set_u16, U16, u16);
    typed!(get_u64, set_u64, U64, u64);

    fn get_str(&self, key: &str) -> Result<Option<String>, Self::Error> {
        Ok(match self.keys.get(key) {
            Some(Value::Str(v)) => Some(v.clone()),
            _ => None,
        })
    }
    fn set_str(&mut self, key: &str, value: &str) -> Result<(), Self::Error> {
        self.set(key, Value::Str(value.to_string()))
    }
    fn erase(&mut self, key: &str) -> Result<(), Self::Error> {
        self.keys.remove(key);
        Ok(())
    }
    fn commit(&mut self) -> Result<(), Self::Error> {
        self.commits += 1;
        Ok(())
    }
}
