//! The board's clock, on virtual time.

use ev::*;

use crate::scheduler::Scheduler;

/// The board's platform Clock on virtual time: a Heartbeat every second,
/// each a Tick with the wall-clock time (`start` plus the time run), a
/// ProgramTick every 60 s, and a Wake at each time a WakeAt asks for. (No
/// fast-day mode: virtual time runs a day as fast as you like.)
pub struct SimClock {
    post: Post,
    scheduler: Scheduler,
    start: TimeReading,
    started: Instant,
    next_update: Option<Instant>,
    wakes: Vec<Instant>, // asked for and not yet come
}

impl SimClock {
    pub const UPDATE_INTERVAL: Duration = Duration::secs(60);

    pub fn new(post: Post, scheduler: Scheduler, start: TimeReading) -> Self {
        SimClock { post, scheduler, start, started: Instant::ZERO, next_update: None, wakes: Vec::new() }
    }

    /// The first Heartbeat a second from now.
    pub fn start(&mut self) {
        self.started = self.scheduler.now();
        self.scheduler.after(Duration::secs(1), Heartbeat { at: self.started + Duration::secs(1) });
    }

    pub fn read(&self) -> TimeReading {
        let mut t = self.start;
        t.local += self.scheduler.now() - self.started;
        t
    }

    fn heartbeat(&mut self, e: &Heartbeat) {
        let time = self.read();
        self.post.send(Tick { at: e.at, time });
        match &mut self.next_update {
            None => self.next_update = Some(e.at + Self::UPDATE_INTERVAL),
            Some(next) if e.at >= *next => {
                *next += Self::UPDATE_INTERVAL;
                self.post.send(ProgramTick { time, transition: Transition::of(Self::UPDATE_INTERVAL) });
            }
            Some(_) => {}
        }
        let next = e.at + Duration::secs(1);
        self.scheduler.at(next, Heartbeat { at: next });
    }

    fn wake_at(&mut self, e: &WakeAt) {
        if self.wakes.contains(&e.at) {
            return; // already asked for
        }
        self.wakes.push(e.at);
        self.scheduler.at(e.at, Wake { at: e.at.max(self.scheduler.now()) });
    }
}

impl Module for SimClock {
    fn on(&mut self, e: &Event) {
        match e {
            Event::Heartbeat(e) => self.heartbeat(e),
            Event::WakeAt(e) => self.wake_at(e),
            Event::Wake(e) => self.wakes.retain(|&w| w > e.at),
            _ => {}
        }
    }
}
