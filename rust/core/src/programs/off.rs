use ev::{LightState, Mode, Role, TimeReading, Transition};

use super::{Base, Program};

/// Every bulb off until another program is chosen (the button's hold).
pub struct OffProgram {
    base: Base,
}

impl OffProgram {
    pub fn new(base: Base) -> Self {
        OffProgram { base }
    }
}

impl Program for OffProgram {
    const MODE: Mode = Mode::Off;

    fn refresh(&mut self, role: Option<Role>, _now: Option<&TimeReading>) {
        for r in self.base.lights(role) {
            self.base.target(r, LightState::default(), Transition::default());
        }
    }
}
