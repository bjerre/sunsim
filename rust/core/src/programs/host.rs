use ev::*;

use super::{Base, DaylightProgram, OffProgram, Program, RedAlertProgram, TuningProgram};

// Each program where program() puts it.
const _: () = assert!(
    matches!(DaylightProgram::MODE, Mode::Daylight)
        && matches!(OffProgram::MODE, Mode::Off)
        && matches!(RedAlertProgram::MODE, Mode::RedAlert)
        && matches!(TuningProgram::MODE, Mode::Tuning),
    "each program is its mode's"
);

/// A zone's programs, one per Mode, and the mode it's in.
struct Zone {
    daylight: DaylightProgram,
    off: OffProgram,
    red_alert: RedAlertProgram,
    tuning: TuningProgram,
    mode: Mode,
}

// One program's methods, for whichever program a mode picks.
trait Run {
    fn refresh(&mut self, role: Option<Role>, now: Option<&TimeReading>);
    fn start(&mut self, now: Option<&TimeReading>);
    fn on(&mut self, e: &Event);
}
impl<P: Program> Run for P {
    fn refresh(&mut self, role: Option<Role>, now: Option<&TimeReading>) {
        Program::refresh(self, role, now);
    }
    fn start(&mut self, now: Option<&TimeReading>) {
        Program::start(self, now);
    }
    fn on(&mut self, e: &Event) {
        Program::on(self, e);
    }
}

impl Zone {
    fn new(post: &Post, zone: ZoneId) -> Self {
        let base = Base::new(post.clone(), zone);
        Zone {
            daylight: DaylightProgram::new(base.clone()),
            off: OffProgram::new(base.clone()),
            red_alert: RedAlertProgram::new(base.clone()),
            tuning: TuningProgram::new(base),
            mode: Mode::Daylight,
        }
    }

    // `mode`'s program.
    fn program(&mut self, mode: Mode) -> &mut dyn Run {
        match mode {
            Mode::Daylight => &mut self.daylight,
            Mode::Off => &mut self.off,
            Mode::RedAlert => &mut self.red_alert,
            Mode::Tuning => &mut self.tuning,
        }
    }

    fn running(&mut self) -> &mut dyn Run {
        self.program(self.mode)
    }

    // `e` to every program.
    fn broadcast(&mut self, e: &Event) {
        for mode in MODES {
            self.program(mode).on(e);
        }
    }
}

/// Owns the lighting programs, a set per zone, and runs the one each zone's
/// Mode selects: it starts it on the zone's ModeChanged, and passes it the
/// ticks and refreshes. Settings, the weather and the console's tuning go to
/// every program (see Program). Keeps the latest time, for programs that
/// need it when they start or refresh. On `tune save` it posts the levels
/// tuned in the zones that are tuning as the lights' minimums.
///
/// Adding a program: a Mode (in ev: the enum, MODES, Mode::name), a Program,
/// its place in Zone here (`program()` won't compile without it), and
/// whatever selects it (a zone's Controls, the console).
pub struct ProgramHost {
    zones: Vec<Zone>,
    post: Post,
    time: Option<TimeReading>, // latest, from Tick/ProgramTick
}

impl ProgramHost {
    pub fn new(post: &Post) -> Self {
        ProgramHost { zones: zone_ids().map(|z| Zone::new(post, z)).collect(), post: post.clone(), time: None }
    }

    fn refresh(&mut self, role: Option<Role>) {
        for zone in &mut self.zones {
            zone.running().refresh(role, self.time.as_ref()); // a program skips lights not in its zone
        }
    }

    fn tune_save(&mut self) {
        for zone in &mut self.zones {
            let tuned = zone.tuning.take_tuned();
            if zone.mode != Mode::Tuning {
                continue; // `tune` reached it, but it isn't tuning: nothing it saw counts
            }
            for role in lights() {
                if let Some(level) = tuned[role] {
                    self.post.send(SetMinLevel { lights: role.into(), level });
                }
            }
        }
    }
}

impl Module for ProgramHost {
    fn on(&mut self, e: &Event) {
        match e {
            Event::ModeChanged(m) => {
                let zone = &mut self.zones[m.zone.0 as usize];
                zone.mode = m.mode;
                zone.running().start(self.time.as_ref());
            }
            Event::Tick(Tick { time, .. }) | Event::ProgramTick(ProgramTick { time, .. }) => {
                self.time = Some(*time);
                for zone in &mut self.zones {
                    zone.running().on(e);
                }
            }
            Event::RefreshLights(r) => self.refresh(r.role),
            Event::NetworkUp(_) => self.refresh(None),
            Event::ConfigChanged(_) => {
                for zone in &mut self.zones {
                    zone.broadcast(e);
                }
                self.refresh(None); // show a change now, not at the next update
            }
            Event::SkyChanged(_) | Event::TuneLights(_) => {
                for zone in &mut self.zones {
                    zone.broadcast(e);
                }
            }
            Event::TuneSave(_) => self.tune_save(),
            _ => {}
        }
    }
}
