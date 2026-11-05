use ev::*;

use super::{Base, Program};
use crate::gate::openness;
use crate::math::daylight::{LookContext, show};

/// Fixed, short transition for changes that should feel immediate rather
/// than mid-animation: resuming, a bulb (re)joining, a setting change.
const SNAP_TRANSITION: Transition = Transition::secs(2);

/// Where a light sits in its group, evenly spaced in letter order: +1 the
/// first, -1 the last (for the wall: east to west; see LookContext).
fn position_in_group(role: Role) -> f64 {
    let count = spec(role.group).count as f64;
    if count < 2.0 { 0.0 } else { 1.0 - 2.0 * role.index as f64 / (count - 1.0) }
}

/// A brightness (0-1) on the light's own range: its minimum level (the
/// lowest it visibly shows) up to its peak. So a light stays lit, dimly,
/// until the curve really ends at sunset.
fn level_for(role: Role, brightness: f64, peaks: &Peaks, min_levels: &MinLevels) -> u8 {
    let top = (MAX_LEVEL as f64 * peaks[role] as f64 / 100.0).max(1.0);
    let floor = (min_levels[role] as f64).min(top);
    (floor + (top - floor) * brightness.clamp(0.0, 1.0)).round() as u8
}

/// What the daylight program has `role` show at `time` with `cloud` cover
/// (0-1): its look, faded by its zone's gate, on its own range. Off with no
/// clock. Shared with what samples the program's curve (the browser
/// simulator's program flow), so they see what the lights will.
pub fn daylight_target(
    role: Role,
    config: &ProgramConfig,
    peaks: &Peaks,
    min_levels: &MinLevels,
    time: &TimeReading,
    cloud: f64,
    installation: &Installation,
) -> LightState {
    let topology = &installation.topology;
    let Some(zone) = topology.zone_of[role].filter(|_| time.valid) else { return LightState::default() };
    let open = openness(&installation.spec(zone).gate, time, config);
    let ctx = LookContext { config, time: *time, position: position_in_group(role), cloud };
    match show(&topology.look_of[role], &ctx) {
        Some(glow) if open > 0.0 => LightState {
            on: true,
            level: level_for(role, glow.brightness * open, peaks, min_levels),
            color: glow.color,
        },
        _ => LightState::default(),
    }
}

/// The normal program: each light shows its look (the installation's: the wall's sun
/// curve, with the cloud cover live, else forecast; the work light's cool ->
/// warm day) while its zone's gate (the active hours) lets it, across the
/// light's range from its minimum level to its peak intensity. Every
/// ProgramTick ramps over the update interval; refreshes and setting
/// changes re-apply at once with a quick transition. Until the clock has
/// been set (no SNTP yet) every bulb is held off.
pub struct DaylightProgram {
    base: Base,
    config: ProgramConfig,
    peaks: Peaks,
    min_levels: MinLevels,
    cloud_percent: u8, // the sky now (none known: clear)
    clock_warned: bool,
}

impl DaylightProgram {
    pub fn new(base: Base) -> Self {
        DaylightProgram {
            base,
            config: ProgramConfig::default(),
            peaks: DEFAULT_PEAKS,
            min_levels: DEFAULT_MIN_LEVELS,
            cloud_percent: 0,
            clock_warned: false,
        }
    }

    fn emit(&mut self, role: Role, time: &TimeReading, transition: Transition) {
        if time.valid {
            self.clock_warned = false;
        } else if !self.clock_warned {
            self.clock_warned = true;
            log::warn!(target: "PROGRAM", "Clock not set yet (no time sync) - holding lights off");
        }
        let cloud = self.cloud_percent as f64 / 100.0;
        let state =
            daylight_target(role, &self.config, &self.peaks, &self.min_levels, time, cloud, self.base.installation());
        self.base.target(role, state, transition);
    }

    fn emit_all(&mut self, role: Option<Role>, time: &TimeReading, transition: Transition) {
        for r in self.base.lights(role) {
            self.emit(r, time, transition);
        }
    }
}

impl Program for DaylightProgram {
    const MODE: Mode = Mode::Daylight;

    fn refresh(&mut self, role: Option<Role>, now: Option<&TimeReading>) {
        // No time yet: the first ProgramTick will apply it.
        if let Some(now) = now {
            self.emit_all(role, now, SNAP_TRANSITION);
        }
    }

    fn on(&mut self, e: &Event) {
        match e {
            Event::ProgramTick(e) => self.emit_all(None, &e.time, e.transition),
            Event::ConfigChanged(e) => {
                self.config = e.config;
                self.peaks = e.peaks;
                self.min_levels = e.min_levels;
            }
            Event::SkyChanged(e) => self.cloud_percent = e.sky.cloud_percent.unwrap_or(0),
            _ => {}
        }
    }
}
