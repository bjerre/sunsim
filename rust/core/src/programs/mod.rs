//! The lighting programs, and ProgramHost, which runs them.
//!
//! A program decides what every light in its zone should show and posts it
//! as LightTargets. It never deals with addresses, reachability or what was
//! last sent; LightOutput does that, the same way for every program.
//! ProgramHost owns one of each per zone and runs the one the zone's Mode
//! selects.

mod daylight;
mod host;
mod off;
mod red_alert;
mod tuning;

pub use daylight::{DaylightProgram, daylight_target};
pub use host::ProgramHost;
pub use off::OffProgram;
pub use red_alert::RedAlertProgram;
pub use tuning::TuningProgram;

use std::sync::Arc;

use ev::{
    Event, Installation, LightChange, LightState, LightTarget, Mode, Post, Role, TimeReading, Transition, ZoneId,
};

/// What a program is to ProgramHost. `now` is None until the clock's first
/// Tick.
pub trait Program {
    /// The mode it is.
    const MODE: Mode;

    /// Re-apply `role` (None = every light) now, with a quick transition: a
    /// light (re)joined or answers again, the network came up, a setting
    /// changed.
    fn refresh(&mut self, role: Option<Role>, now: Option<&TimeReading>);

    /// It became the running program: by default, refresh every light.
    fn start(&mut self, now: Option<&TimeReading>) {
        self.refresh(None, now);
    }

    /// Any events it wants: Tick and ProgramTick reach it only while it
    /// runs; settings, weather and the console's tuning always, so it's
    /// current whenever it starts.
    fn on(&mut self, _e: &Event) {}
}

/// What every program is made with, and does with it: post targets for the
/// lights in its zone.
#[derive(Clone)]
pub struct Base {
    post: Post,
    zone: ZoneId,
    installation: Arc<Installation>,
}

impl Base {
    pub fn new(post: Post, zone: ZoneId, installation: Arc<Installation>) -> Self {
        Base { post, zone, installation }
    }

    pub fn installation(&self) -> &Installation {
        &self.installation
    }

    pub fn zone(&self) -> ZoneId {
        self.zone
    }

    /// The lights in the zone, or only `only` if it's given (and one of them).
    pub fn lights(&self, only: Option<Role>) -> impl Iterator<Item = Role> + use<> {
        let (zone, zone_of) = (self.zone, self.installation.topology.zone_of);
        ev::lights().filter(move |&r| zone_of[r] == Some(zone) && only.is_none_or(|o| o == r))
    }

    pub fn target2(&self, role: Role, state: LightState, level: Transition, color: Transition) {
        self.post.send(LightTarget(LightChange { role, state, level_transition: level, color_transition: color }));
    }
    pub fn target(&self, role: Role, state: LightState, transition: Transition) {
        self.target2(role, state, transition, transition);
    }
}
