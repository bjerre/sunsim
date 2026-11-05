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

/// The normal program: each light shows its look (LOOK_OF: the wall's sun
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

    /// A brightness (0-1) on the light's own range: its minimum level (the
    /// lowest it visibly shows) up to its peak. So a light stays lit, dimly,
    /// until the curve really ends at sunset.
    fn level_for(&self, role: Role, brightness: f64) -> u8 {
        let top = (MAX_LEVEL as f64 * self.peaks[role] as f64 / 100.0).max(1.0);
        let floor = (self.min_levels[role] as f64).min(top);
        (floor + (top - floor) * brightness.clamp(0.0, 1.0)).round() as u8
    }

    fn emit(&mut self, role: Role, time: &TimeReading, transition: Transition) {
        let mut state = LightState::default();
        if time.valid {
            self.clock_warned = false;
            let open = openness(spec_of(self.base.zone()).gate, time, &self.config);
            let ctx = LookContext {
                config: &self.config,
                time: *time,
                position: position_in_group(role),
                cloud: self.cloud_percent as f64 / 100.0,
            };
            if let Some(glow) = show(&LOOK_OF[role], &ctx).filter(|_| open > 0.0) {
                state = LightState { on: true, level: self.level_for(role, glow.brightness * open), color: glow.color };
            }
        } else if !self.clock_warned {
            self.clock_warned = true;
            log::warn!(target: "PROGRAM", "Clock not set yet (no time sync) - holding lights off");
        }
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
