//! Which program each zone runs.

use std::rc::Rc;

use ev::*;

const TAG: &str = "MODE";

/// Which program each zone runs (ProgramHost runs it): the console can ask
/// for one (ModeRequested: `tune`), and a button's presses choose its zone's,
/// as its Controls say (CONTROLS_OF; by default single -> daylight, double ->
/// red alert, hold -> off), posting ModeChanged. Press::Ambiguous (an
/// On/Toggle only seen as an attribute change, from buttons that use the
/// bound receiver endpoint) is resolved by timing: a second one within the
/// double-click window is a double press, otherwise it's a single press as
/// soon as the window has passed (its Timer asks for a Wake then).
pub struct ModeController {
    s: Rc<Shared>,
    _flows: PerRole<Flow>, // a run() per button
}

// What the flows share with the handlers.
struct Shared {
    post: Post,
    window: Duration,
    timer: Timer,
    presses: Inbox<ButtonPressed>,
}

impl ModeController {
    pub const DOUBLE_CLICK_WINDOW: Duration = Duration::millis(450);

    pub fn new(post: Post) -> Self {
        Self::with_window(post, Self::DOUBLE_CLICK_WINDOW)
    }

    pub fn with_window(post: Post, double_click_window: Duration) -> Self {
        let wake = post.clone();
        let timer = Timer::with_wake(move |at| wake.send(WakeAt { at }));
        let s = Rc::new(Shared { post, window: double_click_window, presses: Inbox::new(&timer), timer });
        let mut flows = PerRole::<Flow>::default();
        for role in ROLES.into_iter().filter(|&r| CONTROLS_OF[r].is_some()) {
            flows[role] = Flow::start(run(s.clone(), role));
        }
        ModeController { s, _flows: flows }
    }
}

async fn run(s: Rc<Shared>, button: Role) {
    let from = move |p: &ButtonPressed| p.button == button;
    loop {
        let press = s.presses.next_where(from).await;
        if press.press != Press::Ambiguous {
            s.pressed(button, press.press);
            continue;
        }
        let second = s.presses.next_where_by(from, press.at + s.window).await;
        let press = match second {
            None => Press::Single, // the window passed: it was a single press
            Some(second) if second.press == Press::Ambiguous => Press::Double, // two within the window: a double press
            Some(second) => second.press, // a definite press wins
        };
        s.pressed(button, press);
    }
}

impl Shared {
    // What a definite press on `button` chooses, if anything.
    fn pressed(&self, button: Role, press: Press) {
        let controls = CONTROLS_OF[button].expect("only buttons with controls run");
        let mode = match press {
            Press::Double => controls.twice,
            Press::Hold => controls.hold,
            Press::Single | Press::Ambiguous => controls.single,
        };
        if let Some(mode) = mode {
            self.enter(ZONE_OF[button].expect("a button with controls has a zone"), mode);
        }
    }

    fn enter(&self, zone: ZoneId, mode: Mode) {
        let name = spec_of(zone).name;
        match mode {
            Mode::Daylight => log::info!(target: TAG, "{name}: resuming daylight"),
            Mode::Off => log::info!(target: TAG, "{name}: manual off (press again to resume)"),
            Mode::RedAlert => log::warn!(target: TAG, "{name}: RED ALERT"),
            Mode::Tuning => log::info!(
                target: TAG,
                "{name}: tuning, every light steady at its minimum level (`tune`; press to resume)"
            ),
        }
        self.post.send(ModeChanged { zone, mode });
    }
}

impl Module for ModeController {
    fn on(&mut self, e: &Event) {
        match e {
            Event::ButtonPressed(p) => self.s.presses.deliver(p, Some(p.at)),
            Event::ModeRequested(m) => {
                for zone in zone_ids().filter(|&z| m.zone.is_none_or(|only| only == z) && spec_of(z).allows(m.mode)) {
                    self.s.enter(zone, m.mode);
                }
            }
            Event::Tick(Tick { at, .. }) | Event::Wake(Wake { at }) => self.s.timer.tick(*at), // Wake: the double-click window, ended on time
            _ => {}
        }
    }
}
