//! Everything sunsim persists.

use std::sync::Arc;

use ev::settings::Kind as SettingKind;
use ev::*;

use crate::storage::Storage;

const TAG: &str = "SETTINGS";

/// Pre-versioning format: the whole roles array as one raw-struct blob. Only
/// erased now, never read.
const LEGACY_BLOB_KEY: &str = "roles";

/// The role layout the stored keys follow. Missing: the three-light wall,
/// whose WallA/B/C sat at the east end, middle and west end.
const LAYOUT_KEY: &str = "layout";
const FIVE_LIGHT_WALL: u8 = 1;

/// "<role>.<field>".
fn key(role_name: &str, field: &str) -> String {
    format!("{role_name}.{field}")
}
fn role_key(role: Role, field: &str) -> String {
    key(role_name(role), field)
}

// "<role>.short", the longest, fits NVS's 15 characters.
const _: () = {
    let mut g = 0;
    while g < LAYOUT.len() {
        let name = LAYOUT[g].name.len() + (LAYOUT[g].count > 1) as usize;
        assert!(name + ".short".len() <= 15, "role names must fit NVS keys");
        g += 1;
    }
};

/// Everything sunsim persists, in NVS namespace "sunsim", one key per field
/// so the format survives firmware changes (a new field just reads as its
/// default), as the C++ firmware stores it:
///
/// ```text
/// roles     "<role>.ext" / ".short" / ".ep"   (a role is assigned iff .ext exists)
/// program   SETTINGS' keys: "act.start", "sun.shift", ... (see settings::Kind
///           for how each kind is stored)
/// intensity "<role>.peak" (percent), "<role>.min" (minimum level)
/// "layout"  which role layout the keys follow (see migrate_layout())
/// ```
///
/// Wi-Fi credentials are kept separately.
pub struct SettingsStore<S: Storage> {
    post: Post,
    storage: S,
    config: ProgramConfig,
    peaks: Peaks,
    min_levels: MinLevels,
}

impl<S: Storage> SettingsStore<S> {
    /// `defaults`: the program settings before anything was saved (the
    /// location comes from the build); `default_peaks`: the lights' before
    /// any were set (the installation's).
    pub fn new(post: Post, storage: S, defaults: ProgramConfig, default_peaks: Peaks) -> Self {
        SettingsStore { post, storage, config: defaults, peaks: default_peaks, min_levels: DEFAULT_MIN_LEVELS }
    }

    pub fn storage(&self) -> &S {
        &self.storage
    }
    pub fn storage_mut(&mut self) -> &mut S {
        &mut self.storage
    }

    fn publish(&self) {
        self.post.send(ConfigChanged { config: self.config, peaks: self.peaks, min_levels: self.min_levels });
    }

    /// Posts RolesLoaded and ConfigChanged with what's stored.
    pub fn load_all(&mut self) {
        self.migrate_layout();
        let s = &self.storage;
        // A key that won't read reads as missing: its default.
        let u8_ = |k: &str| s.get_u8(k).ok().flatten();
        let u16_ = |k: &str| s.get_u16(k).ok().flatten();

        let mut records = DeviceRecords::default();
        for role in ROLES {
            let Some(ext_addr) = s.get_u64(&role_key(role, "ext")).ok().flatten() else { continue };
            let d = DeviceRecord {
                valid: true,
                ext_addr,
                short_addr: u16_(&role_key(role, "short")).unwrap_or(0), // refreshed on next announce
                endpoint: u8_(&role_key(role, "ep")).unwrap_or(0),       // 0: rediscovered
            };
            log::info!(
                target: TAG,
                "Restored {}: ext=0x{:016x} short=0x{:04x} ep={}",
                role_name(role),
                d.ext_addr,
                d.short_addr,
                d.endpoint
            );
            records[role] = d;
        }

        // Each setting, if stored and valid.
        for setting in SETTINGS {
            let c = &mut self.config;
            match setting.kind {
                SettingKind::OnOff { field, key } => {
                    if let Some(v) = u8_(key) {
                        (field.set)(c, v != 0);
                    }
                }
                SettingKind::Number { field, min, max, key, .. } => {
                    if let Some(v) = u8_(key).filter(|v| (min..=max).contains(v)) {
                        (field.set)(c, v);
                    }
                }
                SettingKind::Minutes { field, max, key } => {
                    if let Some(v) = u16_(key).map(|v| Duration::minutes(v as i64)).filter(|&v| v <= max) {
                        (field.set)(c, v);
                    }
                }
                SettingKind::Span { field, start_key, end_key } => {
                    if let (Some(start), Some(end)) = (u16_(start_key), u16_(end_key)) {
                        let span = DaySpan::new(Duration::minutes(start as i64), Duration::minutes(end as i64));
                        if span.valid() {
                            (field.set)(c, span);
                        }
                    }
                }
                SettingKind::Days { field, key } => {
                    if let Some(bits) = u8_(key).filter(|&b| b != 0) {
                        (field.set)(c, Weekdays { bits });
                    }
                }
            }
        }
        for role in ROLES {
            if let Some(peak) = u8_(&role_key(role, "peak")).filter(|p| (1..=100).contains(p)) {
                self.peaks[role] = peak;
            }
            if let Some(min) = u8_(&role_key(role, "min")).filter(|m| (1..=MAX_LEVEL).contains(m)) {
                self.min_levels[role] = min;
            }
        }

        self.post.send(RolesLoaded { records: Arc::new(records) });
        self.publish();
    }

    // Writes with `write`, key after key, stopping at the first that fails;
    // commits if all went well, and logs what failed if not.
    fn save(&mut self, what: &str, write: impl FnOnce(&mut S) -> Result<(), S::Error>) {
        let saved = write(&mut self.storage).and_then(|()| self.storage.commit());
        if let Err(e) = saved {
            log::error!(target: TAG, "Failed to save {what}: {e}");
        }
    }

    fn set_config(&mut self, e: &SetConfig) {
        self.config = e.config;
        let c = self.config;
        self.save("settings", |s| {
            for setting in SETTINGS {
                match setting.kind {
                    SettingKind::OnOff { field, key } => s.set_u8(key, (field.get)(&c) as u8)?,
                    SettingKind::Number { field, key, .. } => s.set_u8(key, (field.get)(&c))?,
                    SettingKind::Minutes { field, key, .. } => {
                        s.set_u16(key, (field.get)(&c).whole_minutes() as u16)?
                    }
                    SettingKind::Span { field, start_key, end_key } => {
                        let span = (field.get)(&c);
                        s.set_u16(start_key, span.start.whole_minutes() as u16)?;
                        s.set_u16(end_key, span.end.whole_minutes() as u16)?;
                    }
                    SettingKind::Days { field, key } => s.set_u8(key, (field.get)(&c).bits)?,
                }
            }
            Ok(())
        });
        self.publish();
    }

    // `value` for the lights `lights` selects, in `levels` and under "<role>.<field>".
    fn set_lights(
        &mut self,
        what: &str,
        lights: Selection,
        field: &str,
        value: u8,
        levels: fn(&mut Self) -> &mut PerRole<u8>,
    ) {
        let chosen: Vec<Role> = ev::lights().filter(|&r| selects(lights, r)).collect();
        for &role in &chosen {
            levels(self)[role] = value;
        }
        self.save(what, |s| chosen.iter().try_for_each(|&role| s.set_u8(&role_key(role, field), value)));
        self.publish();
    }

    fn persist_roles(&mut self, records: &DeviceRecords) {
        self.save("roles", |s| {
            s.erase(LEGACY_BLOB_KEY)?;
            for role in ROLES {
                let d = records[role];
                if d.valid {
                    s.set_u64(&role_key(role, "ext"), d.ext_addr)?;
                    s.set_u16(&role_key(role, "short"), d.short_addr)?;
                    s.set_u8(&role_key(role, "ep"), d.endpoint)?;
                } else {
                    for field in ["ext", "short", "ep"] {
                        s.erase(&role_key(role, field))?;
                    }
                }
            }
            Ok(())
        });
    }

    // The three-light wall's WallA/B/C (east end, middle, west end) are the
    // five-light wall's WallA/C/E: the same places. Renames their keys, once.
    fn migrate_layout(&mut self) {
        if self.storage.get_u8(LAYOUT_KEY).ok().flatten().is_some_and(|l| l >= FIVE_LIGHT_WALL) {
            return;
        }
        let s = &mut self.storage;
        let moved_c = move_role(s, "WallC", "WallE"); // first, so WallC is free for WallB
        let moved_b = move_role(s, "WallB", "WallC");
        match s.set_u8(LAYOUT_KEY, FIVE_LIGHT_WALL).and_then(|()| s.commit()) {
            Err(e) => log::error!(target: TAG, "Failed to save the role layout: {e}"),
            Ok(()) if moved_b || moved_c => log::warn!(
                target: TAG,
                "Five-light wall: stored WallB is now WallC, WallC is now WallE (same places on the wall)"
            ),
            Ok(()) => {}
        }
    }
}

// A Storage's getter and setter for one width.
type Get<S, T> = fn(&S, &str) -> Result<Option<T>, <S as Storage>::Error>;
type Set<S, T> = fn(&mut S, &str, T) -> Result<(), <S as Storage>::Error>;

// Moves one key's value to another key, at its width; false if there was none.
fn move_key<S: Storage, T>(s: &mut S, from: &str, to: &str, get: Get<S, T>, set: Set<S, T>) -> bool {
    let Some(value) = get(s, from).ok().flatten() else { return false };
    if set(s, to, value).is_err() {
        return false;
    }
    let _ = s.erase(from);
    true
}

// Moves everything stored for a role to another role's name.
fn move_role<S: Storage>(s: &mut S, from: &str, to: &str) -> bool {
    let mut moved = move_key(s, &key(from, "ext"), &key(to, "ext"), S::get_u64, S::set_u64);
    moved |= move_key(s, &key(from, "short"), &key(to, "short"), S::get_u16, S::set_u16);
    for field in ["ep", "peak", "min"] {
        moved |= move_key(s, &key(from, field), &key(to, field), S::get_u8, S::set_u8);
    }
    moved
}

impl<S: Storage> Module for SettingsStore<S> {
    fn on(&mut self, e: &Event) {
        match e {
            Event::SetConfig(e) => self.set_config(e),
            Event::SetIntensity(e) => {
                self.set_lights("intensity", e.lights, "peak", e.percent.clamp(1, 100), |s| &mut s.peaks)
            }
            Event::SetMinLevel(e) => {
                self.set_lights("minimum level", e.lights, "min", e.level.clamp(1, MAX_LEVEL), |s| &mut s.min_levels)
            }
            Event::PersistRoles(e) => self.persist_roles(&e.records),
            Event::FactoryReset(_) => {
                log::warn!(target: TAG, "Clearing all roles");
                self.persist_roles(&DeviceRecords::default()); // all records invalid -> keys erased
            }
            _ => {}
        }
    }
}
