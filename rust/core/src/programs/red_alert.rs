use ev::{Duration, Event, HueSat, LightState, Mode, Role, TimeReading, Transition};

use super::{Base, Program};

const BEAT: Duration = Duration::secs(1);
const FADE: Transition = Transition::tenths(9); // 0.9 s: nearly the whole beat
const COLOR: Transition = Transition::tenths(2); // 0.2 s
const BRIGHT_LEVEL: u8 = 254;
const DIM_LEVEL: u8 = 30;

/// Every light red, pulsing like TNG's red-alert lighting: a slow, continuous
/// glow rather than a strobe. It flips between bright and dim every second
/// of the Ticks' time (a 2 s cycle, ~0.5 Hz), each fade taking nearly the
/// whole second: a late Tick, or two at once, doesn't change the beat.
/// Starts bright.
pub struct RedAlertProgram {
    base: Base,
    bright: bool,
    beat: Option<i64>, // the second of the last flip
}

impl RedAlertProgram {
    pub fn new(base: Base) -> Self {
        RedAlertProgram { base, bright: true, beat: None }
    }

    fn emit(&self, role: Option<Role>) {
        let red = LightState {
            on: true,
            level: if self.bright { BRIGHT_LEVEL } else { DIM_LEVEL },
            color: HueSat { hue: 0, saturation: 254 }.into(),
        };
        for r in self.base.lights(role) {
            self.base.target2(r, red, FADE, COLOR);
        }
    }
}

impl Program for RedAlertProgram {
    const MODE: Mode = Mode::RedAlert;

    fn start(&mut self, _now: Option<&TimeReading>) {
        self.bright = true;
        self.beat = None;
        self.emit(None);
    }

    fn refresh(&mut self, role: Option<Role>, _now: Option<&TimeReading>) {
        self.emit(role);
    }

    fn on(&mut self, e: &Event) {
        if let Event::Tick(e) = e {
            let beat = e.at.since_boot() / BEAT;
            if self.beat == Some(beat) {
                return; // a second Tick in the same second
            }
            self.beat = Some(beat);
            self.bright = !self.bright;
            self.emit(None);
        }
    }
}
