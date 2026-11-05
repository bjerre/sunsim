//! Time for the whole app.

use ev::*;

const TAG: &str = "CLOCK";

const UPDATE_INTERVAL: Duration = Duration::secs(60);
const FAST_DAY_UPDATE_INTERVAL: Duration = Duration::secs(1); // the gradient visibly animates

/// SNTP-synced time is always well past this; an unsynced clock starts at 1970.
const MIN_VALID_YEAR: i32 = 2024;

/// Compresses a day into a short, continuously-looping real-time window -
/// for bench-testing the daylight program without waiting for an actual
/// sunrise/sunset. now() reflects the time since it was made (`start`).
///
/// Only the active window is simulated (ProgramConfig's active hours and
/// days, read live on each call so `config` changes show up on the next
/// loop): each loop runs through the active hours, and successive loops step
/// through the active days only, Monday first, so no loop time is spent on
/// hours or days where the lights are off anyway. The dates are those days
/// of the first week of 2024 (Monday the 1st): a winter sun.
#[derive(Clone, Copy, Debug)]
pub struct FastDayClock {
    real_day: Duration,
    start: Instant,
}

impl FastDayClock {
    const FIRST_MONDAY: LocalDays = LocalDays::ymd(2024, 1, 1);

    pub fn new(real_day_length: Duration, start: Instant) -> Self {
        FastDayClock { real_day: real_day_length, start }
    }

    pub fn now(&self, config: &ProgramConfig, now: Instant) -> LocalSeconds {
        let elapsed = now - self.start;
        let through = (elapsed % self.real_day).as_secs_f64() / self.real_day.as_secs_f64(); // this loop, 0 to 1
        let hours = config.active.end - config.active.start;
        let time = config.active.start + Duration::secs((hours.as_secs_f64() * through).round() as i64);
        Self::day(config.active_days, elapsed / self.real_day) + time
    }

    // The loop-th active day, cycling Monday first.
    fn day(active: Weekdays, loop_: i64) -> LocalDays {
        let week: Vec<LocalDays> =
            (0..7).map(|i| LocalDays(Self::FIRST_MONDAY.0 + i)).filter(|d| active.contains(d.weekday())).collect();
        if week.is_empty() { Self::FIRST_MONDAY } else { week[loop_ as usize % week.len()] }
    }
}

/// What the Clock needs of the board: its two clocks, the time zone, and a
/// one-shot timer for Wakes. (The 1 Hz Heartbeat it posts by itself.)
pub trait ClockHardware {
    /// Steady time since boot.
    fn now(&self) -> Instant;
    /// The wall clock, UTC: 1970 until it's set.
    fn wall(&self) -> SysSeconds;
    /// The local UTC offset at `at` (zone::utc_offset of its local time).
    fn utc_offset(&self, at: SysSeconds) -> Duration;
    /// Post Wake at `at` (once), instead of at any time armed before; None:
    /// none.
    fn arm_wake(&mut self, at: Option<Instant>);
}

/// Time for the whole app. The board's 1 Hz timer posts Heartbeat, stamped
/// when it fired; the Clock (app task) reads the time and posts Tick at
/// that time, plus ProgramTick every update interval: 60 s, or 1 s in the
/// debug fast-day mode, whose synthetic clock loops the configured active
/// window. Fast-day is switched at runtime (SetFastDay, console `fastday`;
/// the build picks how it boots). The real clock is kept set by
/// InternetSync. A WakeAt arms the one-shot timer, which posts Wake then,
/// for flows that can't wait for the next Tick.
pub struct Clock<H: ClockHardware> {
    post: Post,
    hw: H,
    config: ProgramConfig,
    fast_day: Option<FastDayClock>, // in fast-day mode
    update_interval: Duration,      // 1 s in fast-day mode
    next_update: Option<Instant>,   // the next ProgramTick; none until a Heartbeat sets it
    wakes: Vec<Instant>,            // asked for (WakeAt), soonest first
}

impl<H: ClockHardware> Clock<H> {
    /// `fast_day`: boot in fast-day mode, a loop this long.
    pub fn new(post: Post, hw: H, fast_day: Option<Duration>) -> Self {
        let mut clock = Clock {
            post,
            config: ProgramConfig::default(),
            fast_day: None,
            update_interval: UPDATE_INTERVAL,
            next_update: None,
            wakes: Vec::new(),
            hw,
        };
        if let Some(day) = fast_day {
            clock.fast_day = Some(FastDayClock::new(day, clock.hw.now()));
            clock.update_interval = FAST_DAY_UPDATE_INTERVAL;
            log::warn!(
                target: TAG,
                "DEBUG FAST DAY MODE: one active window every {} s (`fastday off` for real time)",
                day.floor_secs()
            );
        }
        clock
    }

    pub fn hardware(&mut self) -> &mut H {
        &mut self.hw
    }

    pub fn read(&self) -> TimeReading {
        let wall = self.hw.wall();
        let utc_offset = self.hw.utc_offset(wall);
        match &self.fast_day {
            Some(fast) => {
                TimeReading { local: fast.now(&self.config, self.hw.now()), utc_offset, valid: true, simulated: true }
            }
            None => {
                let local = LocalSeconds(wall.0) + utc_offset;
                TimeReading { local, utc_offset, valid: local.day().civil().0 >= MIN_VALID_YEAR, simulated: false }
            }
        }
    }

    fn heartbeat(&mut self, e: &Heartbeat) {
        let time = self.read();
        self.post.send(Tick { at: e.at, time });
        match &mut self.next_update {
            None => self.next_update = Some(e.at + self.update_interval),
            Some(next) if e.at >= *next => {
                // On schedule rather than drifting by how late each Heartbeat
                // was handled; after a long stall, from now.
                *next += self.update_interval;
                if *next <= e.at {
                    *next = e.at + self.update_interval;
                }
                // Ramp over the whole interval, so bulbs fade continuously from
                // one update to the next instead of jumping and holding.
                self.post.send(ProgramTick { time, transition: Transition::of(self.update_interval) });
            }
            Some(_) => {}
        }
    }

    fn wake_at(&mut self, at: Instant) {
        match self.wakes.binary_search(&at) {
            Ok(_) => {} // already asked for
            Err(i) => {
                self.wakes.insert(i, at);
                self.arm_wake();
            }
        }
    }

    // The one-shot timer, for the soonest wake asked for.
    fn arm_wake(&mut self) {
        self.hw.arm_wake(self.wakes.first().copied());
    }

    fn set_fast_day(&mut self, e: &SetFastDay) {
        let was_on = self.fast_day.is_some();
        if e.on {
            self.fast_day = Some(FastDayClock::new(e.day_length, self.hw.now())); // (re)starts the loop
            self.update_interval = FAST_DAY_UPDATE_INTERVAL;
            log::warn!(target: TAG, "Fast-day mode: one active window every {} s", e.day_length.floor_secs());
        } else if was_on {
            self.fast_day = None;
            self.update_interval = UPDATE_INTERVAL;
            log::info!(target: TAG, "Fast-day mode off: real time");
        } else {
            return; // already off
        }
        self.next_update = None; // the new interval, from the next Heartbeat

        // Show the new time now, with the quick transition, rather than at the
        // next update: programs refresh from the latest Tick's time.
        self.post.send(Tick { at: self.hw.now(), time: self.read() });
        self.post.send(RefreshLights { role: None });
        if e.on != was_on {
            self.post.send(SyncRequested); // switching modes: a moment to sync and fetch
        }
    }
}

impl<H: ClockHardware> Module for Clock<H> {
    fn on(&mut self, e: &Event) {
        match e {
            Event::Heartbeat(e) => self.heartbeat(e),
            Event::WakeAt(e) => self.wake_at(e.at),
            Event::Wake(e) => {
                self.wakes.retain(|&w| w > e.at);
                self.arm_wake();
            }
            Event::ConfigChanged(e) => self.config = e.config,
            Event::SetFastDay(e) => self.set_fast_day(e),
            _ => {}
        }
    }
}
