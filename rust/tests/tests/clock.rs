//! The platform's Clock, on a fake board: Ticks, ProgramTicks, Wakes and fast-day.

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use ev::*;
use platform::{Clock, ClockHardware, FastDayClock, zone};
use sunsim_tests::*;

// 2026-06-23 17:00 UTC: noon in a zone five hours behind. (The clock calls
// anything before 2024 unset, so not noon()'s 2022.)
const WALL_NOON: SysSeconds = SysSeconds(days_from_civil(2026, 6, 23) * 86_400 + 17 * 3600);

#[derive(Clone, Default)]
struct Board {
    now: Rc<Cell<Instant>>,
    wall: Rc<Cell<SysSeconds>>,
    armed: Rc<RefCell<Vec<Option<Instant>>>>,
}

impl ClockHardware for Board {
    fn now(&self) -> Instant {
        self.now.get()
    }
    fn wall(&self) -> SysSeconds {
        self.wall.get()
    }
    fn utc_offset(&self, _: SysSeconds) -> Duration {
        -hours(5)
    }
    fn arm_wake(&mut self, at: Option<Instant>) {
        self.armed.borrow_mut().push(at);
    }
}

// A Clock on the bus, alone, with a log.
struct Rig {
    bus: EventBus,
    board: Board,
    log: Rc<RefCell<Vec<Event>>>,
}

impl Rig {
    fn new(fast_day: Option<Duration>) -> Self {
        let mut bus = EventBus::new();
        let board = Board::default();
        board.wall.set(WALL_NOON);
        let clock = Rc::new(RefCell::new(Clock::new(bus.post().clone(), board.clone(), fast_day)));
        bus.attach(&clock);
        let log = Rc::new(RefCell::new(Vec::new()));
        let recorder = log.clone();
        bus.on_any(move |e| recorder.borrow_mut().push(e.clone()));
        Rig { bus, board, log }
    }

    fn post(&mut self, e: impl Into<Event>) {
        self.bus.post().send(e);
        self.bus.drain();
    }

    // `n` Heartbeats a second apart, the board's time moving with them.
    fn beat(&mut self, n: u32) {
        for _ in 0..n {
            let at = self.board.now.get() + secs(1);
            self.board.now.set(at);
            self.board.wall.set(self.board.wall.get() + secs(1));
            self.post(Heartbeat { at });
        }
    }

    fn all<T: Payload>(&self) -> Vec<T> {
        self.log.borrow().iter().filter_map(T::of).cloned().collect()
    }
}

#[test]
fn heartbeats_become_ticks_with_the_local_time_and_a_program_tick_a_minute() {
    let mut r = Rig::new(None);
    r.beat(1);
    let ticks = r.all::<Tick>();
    assert_eq!(ticks.len(), 1);
    assert!(ticks[0].time.valid && !ticks[0].time.simulated);
    assert_eq!(ticks[0].time.local, LocalDays::ymd(2026, 6, 23) + hours(12) + secs(1));
    assert_eq!(ticks[0].time.utc_offset, -hours(5));

    r.beat(120);
    let updates = r.all::<ProgramTick>();
    assert_eq!(updates.len(), 2); // 60 s after the first Heartbeat, then 60 s on
    assert_eq!(updates[0].transition, Transition::secs(60));
}

#[test]
fn after_a_stall_program_ticks_go_on_from_now_not_in_a_burst() {
    let mut r = Rig::new(None);
    r.beat(1);
    r.board.now.set(r.board.now.get() + Duration::minutes(10)); // the app task was busy
    r.beat(1);
    assert_eq!(r.all::<ProgramTick>().len(), 1);
    r.beat(59);
    assert_eq!(r.all::<ProgramTick>().len(), 1);
    r.beat(1);
    assert_eq!(r.all::<ProgramTick>().len(), 2);
}

#[test]
fn an_unset_clock_reads_invalid() {
    let mut r = Rig::new(None);
    r.board.wall.set(SysSeconds(30)); // 1970
    r.post(Heartbeat { at: Instant::ZERO });
    assert!(!r.all::<Tick>()[0].time.valid);
}

#[test]
fn wakes_arm_the_soonest_asked_for_once_each() {
    let mut r = Rig::new(None);
    let at = |s| Instant::from_boot(secs(s));
    r.post(WakeAt { at: at(5) });
    r.post(WakeAt { at: at(3) });
    r.post(WakeAt { at: at(5) }); // already asked for
    r.post(Wake { at: at(3) });
    r.post(Wake { at: at(5) });
    assert_eq!(*r.board.armed.borrow(), [Some(at(5)), Some(at(3)), Some(at(5)), None]);
}

#[test]
fn fast_day_loops_the_active_hours_on_the_active_days() {
    let config = ProgramConfig::default(); // 08:00-19:00, Monday to Friday
    let start = Instant::from_boot(secs(100));
    let fast = FastDayClock::new(secs(120), start);
    let at = |s| fast.now(&config, start + secs(s));
    assert_eq!(at(0), LocalDays::ymd(2024, 1, 1) + hours(8)); // Monday, as the hours start
    assert_eq!(at(60), LocalDays::ymd(2024, 1, 1) + hours(13) + mins(30)); // half way through them
    assert_eq!(at(120), LocalDays::ymd(2024, 1, 2) + hours(8)); // Tuesday
    assert_eq!(at(5 * 120), LocalDays::ymd(2024, 1, 1) + hours(8)); // Friday's done: Monday again
}

#[test]
fn switching_fast_day_shows_the_new_time_at_once_and_asks_for_a_sync() {
    let mut r = Rig::new(None);
    r.beat(1);
    r.log.borrow_mut().clear();
    r.post(SetFastDay { on: true, day_length: secs(120) });
    let ticks = r.all::<Tick>();
    assert!(ticks.len() == 1 && ticks[0].time.simulated);
    assert_eq!(r.all::<RefreshLights>().len(), 1);
    assert_eq!(r.all::<SyncRequested>().len(), 1);

    r.beat(2); // a ProgramTick a second now
    assert_eq!(r.all::<ProgramTick>().len(), 1);
    assert_eq!(r.all::<ProgramTick>()[0].transition, Transition::secs(1));

    r.log.borrow_mut().clear();
    r.post(SetFastDay { on: false, ..SetFastDay::default() });
    r.post(SetFastDay { on: false, ..SetFastDay::default() }); // already off: nothing
    assert_eq!(r.all::<SyncRequested>().len(), 1);
    assert!(!r.all::<Tick>()[0].time.simulated);
}

#[test]
fn the_utc_offset_is_the_local_fields_read_as_utc_less_the_instant() {
    // 17:00 UTC is 12:00 local, five hours behind; and 01:30 the next day, two and a half ahead.
    assert_eq!(zone::utc_offset(WALL_NOON, (2026, 6, 23, 12, 0, 0)), -hours(5));
    assert_eq!(zone::utc_offset(WALL_NOON, (2026, 6, 23, 19, 30, 0)), hours(2) + mins(30));
}
