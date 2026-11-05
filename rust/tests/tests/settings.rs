//! The platform's SettingsStore on an in-memory NVS: what's kept, under
//! which keys and at which widths (the C++ firmware's, so a board keeps its
//! settings across the switch), and what's read back.

use std::cell::RefCell;
use std::rc::Rc;

use ev::*;
use platform::SettingsStore;
use sunsim_tests::*;

type Store = Rc<RefCell<SettingsStore<Nvs>>>;

// A SettingsStore on the bus, alone, with a log.
struct Rig {
    bus: EventBus,
    store: Store,
    log: Rc<RefCell<Vec<Event>>>,
}

impl Rig {
    fn new(nvs: Nvs) -> Self {
        let mut bus = EventBus::new();
        let defaults = ProgramConfig { latitude_deg: 44.98, ..ProgramConfig::default() };
        let store = Rc::new(RefCell::new(SettingsStore::new(bus.post().clone(), nvs, defaults)));
        bus.attach(&store);
        let log = Rc::new(RefCell::new(Vec::new()));
        let recorder = log.clone();
        bus.on_any(move |e| recorder.borrow_mut().push(e.clone()));
        Rig { bus, store, log }
    }

    fn load(&mut self) {
        self.store.borrow_mut().load_all();
        self.bus.drain();
    }

    fn post(&mut self, e: impl Into<Event>) {
        self.bus.post().send(e);
        self.bus.drain();
    }

    fn all<T: Payload>(&self) -> Vec<T> {
        self.log.borrow().iter().filter_map(T::of).cloned().collect()
    }
    fn last_config(&self) -> ConfigChanged {
        self.all::<ConfigChanged>().pop().expect("a ConfigChanged")
    }
    fn key(&self, key: &str) -> Option<Value> {
        self.store.borrow().storage().keys.get(key).cloned()
    }

    // What's stored, loaded by a fresh store: as after a restart.
    fn restart(self) -> Rig {
        let keys = self.store.borrow().storage().keys.clone();
        let mut r = Rig::new(Nvs { keys, ..Nvs::default() });
        r.load();
        r
    }
}

#[test]
fn nothing_stored_the_defaults_and_no_roles() {
    let mut r = Rig::new(Nvs::default());
    r.load();
    let roles = r.all::<RolesLoaded>();
    assert!(roles.len() == 1 && roles[0].records.0.iter().all(|d| !d.valid));
    let c = r.last_config();
    assert_eq!(c.config, ProgramConfig { latitude_deg: 44.98, ..ProgramConfig::default() }); // the build's location kept
    assert!(c.peaks == DEFAULT_PEAKS && c.min_levels == DEFAULT_MIN_LEVELS);
    assert_eq!(r.key("layout"), Some(Value::U8(1))); // the layout recorded, once
}

#[test]
fn roles_are_kept_under_the_c_plus_plus_keys_and_come_back() {
    let mut r = Rig::new(Nvs::default());
    r.load();
    let mut records = DeviceRecords::default();
    records[WALL_C] = DeviceRecord { valid: true, ext_addr: 0x00124B0001020304, short_addr: 0x1234, endpoint: 11 };
    records[BUTTON] = DeviceRecord { valid: true, ext_addr: 0xAB, short_addr: 0x2FED, endpoint: 0 };
    r.post(PersistRoles { records: std::sync::Arc::new(records) });
    assert_eq!(r.key("WallC.ext"), Some(Value::U64(0x00124B0001020304)));
    assert_eq!(r.key("WallC.short"), Some(Value::U16(0x1234)));
    assert_eq!(r.key("WallC.ep"), Some(Value::U8(11)));
    assert_eq!(r.key("Button.short"), Some(Value::U16(0x2FED)));

    let r = r.restart();
    let loaded = &r.all::<RolesLoaded>()[0].records;
    assert_eq!(loaded[WALL_C], records[WALL_C]);
    assert_eq!(loaded[BUTTON], records[BUTTON]);
    assert!(!loaded[WALL_A].valid);
}

#[test]
fn unpairing_and_factory_reset_erase_a_roles_keys_and_the_legacy_blob() {
    let nvs = Nvs::default()
        .with("roles", Value::U8(0)) // the pre-versioning blob (its type doesn't matter here)
        .with("WallA.ext", Value::U64(0xA1))
        .with("WallA.short", Value::U16(1))
        .with("WallA.ep", Value::U8(1))
        .with("WallA.peak", Value::U8(50));
    let mut r = Rig::new(nvs);
    r.load();
    r.post(FactoryReset);
    for key in ["roles", "WallA.ext", "WallA.short", "WallA.ep"] {
        assert_eq!(r.key(key), None, "{key}");
    }
    assert_eq!(r.key("WallA.peak"), Some(Value::U8(50))); // a light's intensity isn't its pairing
}

#[test]
fn settings_are_saved_at_their_widths_and_come_back() {
    let mut r = Rig::new(Nvs::default());
    r.load();
    let mut config = r.last_config().config;
    config.active = DaySpan::new(hours(7) + mins(30), hours(24));
    config.active_days = Weekdays { bits: 0b1000010 }; // mon, sat
    config.fade = mins(20);
    config.sun_shift_percent = 40;
    config.amber_enabled = false;
    r.post(SetConfig { config });
    assert_eq!(r.key("act.start"), Some(Value::U16(450)));
    assert_eq!(r.key("act.end"), Some(Value::U16(1440)));
    assert_eq!(r.key("act.days"), Some(Value::U8(0b1000010)));
    assert_eq!(r.key("act.fade"), Some(Value::U16(20)));
    assert_eq!(r.key("sun.shift"), Some(Value::U8(40)));
    assert_eq!(r.key("sun.amber"), Some(Value::U8(0)));
    assert_eq!(r.last_config().config, config); // published at once

    let r = r.restart();
    assert_eq!(r.last_config().config, config);
}

#[test]
fn stored_values_out_of_range_or_at_the_wrong_width_read_as_their_defaults() {
    let nvs = Nvs::default()
        .with("sun.shift", Value::U8(101)) // 0-100
        .with("act.start", Value::U16(19 * 60)) // backwards: 19:00-08:00
        .with("act.end", Value::U16(8 * 60))
        .with("act.days", Value::U8(0)) // no days
        .with("act.fade", Value::U8(5)) // a u16 setting, stored as a u8
        .with("sky.dim", Value::U8(70)) // fine
        .with("WallA.peak", Value::U8(0)) // 1-100
        .with("Work.min", Value::U8(255)); // 1-254
    let mut r = Rig::new(nvs);
    r.load();
    let c = r.last_config();
    let d = ProgramConfig::default();
    assert_eq!(c.config.sun_shift_percent, d.sun_shift_percent);
    assert_eq!(c.config.active, d.active);
    assert_eq!(c.config.active_days, d.active_days);
    assert_eq!(c.config.fade, d.fade);
    assert_eq!(c.config.cloud_dim_percent, 70);
    assert!(c.peaks == DEFAULT_PEAKS && c.min_levels == DEFAULT_MIN_LEVELS);
}

#[test]
fn intensity_and_minimum_levels_go_to_the_selected_lights_clamped() {
    let mut r = Rig::new(Nvs::default());
    r.load();
    r.post(SetIntensity { lights: Group::Wall.into(), percent: 0 }); // at least 1
    r.post(SetMinLevel { lights: Selection::AllRoles, level: 255 }); // at most 254
    let c = r.last_config();
    assert!(members(Group::Wall).iter().all(|&w| c.peaks[w] == 1));
    assert_eq!(c.peaks[WORK], DEFAULT_PEAKS[WORK]);
    assert!(lights().all(|l| c.min_levels[l] == MAX_LEVEL));
    assert_eq!(r.key("WallE.peak"), Some(Value::U8(1)));
    assert_eq!(r.key("Work.min"), Some(Value::U8(254)));
    assert_eq!(r.key("Button.min"), None); // a button isn't a light

    let r = r.restart();
    assert_eq!(r.last_config().peaks, c.peaks);
}

#[test]
fn the_three_light_walls_roles_move_to_the_same_places_on_the_five_light_wall_once() {
    // As a board stored them before the wall had five lights: east, middle, west.
    let nvs = Nvs::default()
        .with("WallA.ext", Value::U64(0xA))
        .with("WallB.ext", Value::U64(0xB))
        .with("WallB.peak", Value::U8(60))
        .with("WallC.ext", Value::U64(0xC))
        .with("WallC.ep", Value::U8(11));
    let mut r = Rig::new(nvs);
    r.load();
    let roles = &r.all::<RolesLoaded>()[0].records;
    assert_eq!(roles[WALL_A].ext_addr, 0xA);
    assert_eq!(roles[WALL_C].ext_addr, 0xB); // the middle
    assert_eq!(roles[WALL_E].ext_addr, 0xC); // the west end
    assert!(roles[WALL_E].endpoint == 11 && !roles[WALL_B].valid && !roles[WALL_D].valid);
    assert_eq!(r.last_config().peaks[WALL_C], 60);

    // Once: a WallB paired since stays WallB.
    let mut records = *r.all::<RolesLoaded>()[0].records;
    records[WALL_B] = DeviceRecord { valid: true, ext_addr: 0xBB, short_addr: 2, endpoint: 1 };
    r.post(PersistRoles { records: std::sync::Arc::new(records) });
    let r = r.restart();
    assert_eq!(r.all::<RolesLoaded>()[0].records[WALL_B].ext_addr, 0xBB);
}

#[test]
fn a_failed_save_is_logged_not_committed_and_the_change_still_applies() {
    let mut r = Rig::new(Nvs::default());
    r.load();
    let commits = r.store.borrow().storage().commits;
    r.store.borrow_mut().storage_mut().full = true;
    r.post(SetIntensity { lights: WALL_A.into(), percent: 70 });
    assert_eq!(r.store.borrow().storage().commits, commits);
    assert_eq!(r.last_config().peaks[WALL_A], 70); // it shows, until a restart
}
