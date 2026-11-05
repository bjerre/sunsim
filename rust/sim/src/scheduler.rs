//! Virtual time.

use std::cell::RefCell;
use std::cmp::Ordering;
use std::collections::BinaryHeap;
use std::rc::Rc;

use ev::{Duration, Event, EventBus, Instant};

struct Due {
    at: Instant,
    order: u64,
    event: Event,
}

// For the heap: the soonest on top, and of those the first scheduled.
impl Ord for Due {
    fn cmp(&self, other: &Self) -> Ordering {
        (other.at, other.order).cmp(&(self.at, self.order))
    }
}
impl PartialOrd for Due {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl PartialEq for Due {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}
impl Eq for Due {}

#[derive(Default)]
struct State {
    now: Instant,
    scheduled: u64,
    due: BinaryHeap<Due>,
}

/// Virtual time for a simulated run: events due at an instant, posted to the
/// bus when the run gets there, with everything they set off handled before
/// time moves on. Nothing waits: a day runs as fast as its events can be
/// handled, and the same way every time. Clones are the same schedule.
#[derive(Clone, Default)]
pub struct Scheduler(Rc<RefCell<State>>);

impl Scheduler {
    pub fn new() -> Self {
        Scheduler::default()
    }

    pub fn now(&self) -> Instant {
        self.0.borrow().now
    }

    /// `e` at `when` (at once, if that's past). Those due at the same instant
    /// go in the order they were scheduled.
    pub fn at(&self, when: Instant, e: impl Into<Event>) {
        let mut st = self.0.borrow_mut();
        let (at, order) = (when.max(st.now), st.scheduled);
        st.scheduled += 1;
        st.due.push(Due { at, order, event: e.into() });
    }
    pub fn after(&self, delay: Duration, e: impl Into<Event>) {
        self.at(self.now() + delay, e);
    }

    /// Everything due by `until`, in time order, through `bus`; then the time is `until`.
    pub fn run_until(&self, bus: &mut EventBus, until: Instant) {
        bus.drain();
        loop {
            let next = {
                let mut st = self.0.borrow_mut();
                if st.due.peek().is_none_or(|d| d.at > until) {
                    break;
                }
                let next = st.due.pop().unwrap();
                st.now = next.at;
                next
            };
            bus.post().send(next.event);
            bus.drain();
        }
        let mut st = self.0.borrow_mut();
        st.now = st.now.max(until);
    }
    pub fn run_for(&self, bus: &mut EventBus, duration: Duration) {
        self.run_until(bus, self.now() + duration);
    }
}
