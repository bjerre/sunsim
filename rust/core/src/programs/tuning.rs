use ev::{
    Color, DEFAULT_MIN_LEVELS, Event, LightState, MAX_LEVEL, MinLevels, Mireds, Mode, PerRole, Role, TimeReading,
    Transition, selects,
};

use super::{Base, Program};
use crate::math::daylight::planckian_xy;

/// For finding the lowest level each bulb visibly shows (bulbs differ, and
/// below it they go dark before sunset). Holds every bulb steady: at its
/// minimum level, in the sunset's amber, until the console (`tune`) sets a
/// level or color. On `tune save`, ProgramHost takes the tuned levels and
/// makes them the bulbs' minimums (SetMinLevel), which the daylight curve
/// then never goes below while on.
pub struct TuningProgram {
    base: Base,
    min_levels: MinLevels,
    tuned: PerRole<Option<u8>>, // set with `tune`, not yet saved
    kelvin: f64,                // amber_kelvin until `tune` gives one
}

impl TuningProgram {
    pub fn new(base: Base) -> Self {
        TuningProgram { base, min_levels: DEFAULT_MIN_LEVELS, tuned: PerRole::default(), kelvin: 1800.0 }
    }

    /// The levels set with `tune` since the last take, and forgets them.
    pub fn take_tuned(&mut self) -> PerRole<Option<u8>> {
        std::mem::take(&mut self.tuned)
    }

    fn emit(&self, role: Role) {
        // Like the sunset, where bulbs go dark first: xy below ~2700K.
        let color: Color = if self.kelvin < 2700.0 {
            planckian_xy(self.kelvin).into()
        } else {
            Mireds((1e6 / self.kelvin).round() as u16).into()
        };
        let level = self.tuned[role].unwrap_or(self.min_levels[role]);
        self.base.target(role, LightState { on: true, level, color }, Transition::secs(1));
    }
}

impl Program for TuningProgram {
    const MODE: Mode = Mode::Tuning;

    fn start(&mut self, now: Option<&TimeReading>) {
        self.tuned = PerRole::default();
        self.refresh(None, now);
    }

    fn refresh(&mut self, role: Option<Role>, _now: Option<&TimeReading>) {
        for r in self.base.lights(role) {
            self.emit(r);
        }
    }

    fn on(&mut self, e: &Event) {
        match e {
            Event::ConfigChanged(e) => {
                self.min_levels = e.min_levels;
                self.kelvin = e.config.amber_kelvin;
            }
            Event::TuneLights(e) => {
                if let Some(kelvin) = e.kelvin {
                    self.kelvin = kelvin as f64;
                }
                for r in self.base.lights(None).filter(|&r| selects(e.lights, r)) {
                    self.tuned[r] = Some(e.level.clamp(1, MAX_LEVEL));
                }
                self.refresh(None, None); // a color change applies to every bulb
            }
            _ => {}
        }
    }
}
