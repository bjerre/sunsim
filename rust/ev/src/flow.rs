//! Flows: behavior that happens in steps over time ("send this, wait for
//! the reply or 15 s, try again"), written top to bottom as an async block
//! instead of spread over handlers, flags and counters. Everything runs on
//! the app task, so nothing here locks or is `Send`.
//!
//! ```ignore
//! struct Registry { s: Rc<Shared>, asking: Flow }  // reassigning or dropping `asking` cancels it
//! struct Shared { post: Post, timer: Timer, replies: Inbox<Reply>, state: RefCell<State> }
//!
//! impl Registry {
//!     fn on_reply(&mut self, r: &Reply) { self.s.replies.deliver(r, None); }
//!     fn on_tick(&mut self, t: &Tick)   { self.s.timer.tick(t.at); }
//!     fn ask(&mut self, role: Role) { self.asking = Flow::start(ask(self.s.clone(), role)); }
//! }
//!
//! async fn ask(s: Rc<Shared>, role: Role) {
//!     loop {
//!         s.post.send(Request { role });
//!         if let Some(reply) = s.replies.next_where_by(for_role(role), s.timer.now() + Duration::secs(15)).await {
//!             return s.use_reply(reply);
//!         }
//!     }
//! }
//! ```
//!
//! A flow can't borrow the module that owns it, so what it shares with the
//! module's handlers lives behind an `Rc` (the module's "shared" half), with
//! its mutable state in a `RefCell` that neither side holds across an
//! `.await` or a call that resumes flows (Timer::tick, Inbox::deliver).
//!
//! There's no executor: like a C++ coroutine, a flow runs as soon as it's
//! started, until it first waits, and the Timer or Inbox it waits on resumes
//! it right there, in tick() or deliver(). Only their futures may be awaited
//! in a flow (they find the flow they're in themselves; the `Waker` is unused).

use std::cell::{Cell, RefCell};
use std::future::Future;
use std::pin::Pin;
use std::rc::{Rc, Weak};
use std::task::{Context, Poll, Waker};

use crate::time::{Duration, Instant};

type Body = Pin<Box<dyn Future<Output = ()>>>;
type Filter<T> = Box<dyn Fn(&T) -> bool>;

struct Task {
    body: RefCell<Option<Body>>, // out while it runs; None once done or canceled
    running: Cell<bool>,
    again: Cell<bool>,    // resumed while running: run it again after
    canceled: Cell<bool>, // its Flow was dropped while it ran
}

thread_local! {
    // The flow running now, for the futures it awaits to register.
    static CURRENT: RefCell<Option<Weak<Task>>> = const { RefCell::new(None) };
}

fn current() -> Weak<Task> {
    CURRENT.with_borrow(|c| c.clone()).expect("a Timer or Inbox awaited outside a Flow")
}

impl Task {
    // Runs the flow until it waits (or ends).
    fn resume(self: &Rc<Self>) {
        if self.running.replace(true) {
            self.again.set(true);
            return;
        }
        loop {
            self.again.set(false);
            let Some(mut body) = self.body.borrow_mut().take() else {
                break;
            };
            let outer = CURRENT.replace(Some(Rc::downgrade(self)));
            let done = body.as_mut().poll(&mut Context::from_waker(Waker::noop())).is_ready();
            CURRENT.set(outer);
            if done || self.canceled.get() {
                drop(body);
                break;
            }
            *self.body.borrow_mut() = Some(body);
            if !self.again.get() {
                break;
            }
        }
        self.running.set(false);
    }
}

/// A flow that starts at once and runs until it returns or its owner drops
/// it, which cancels it wherever it's waiting. Nobody awaits it.
#[derive(Default)]
pub struct Flow(Option<Rc<Task>>);

impl Flow {
    pub fn start(body: impl Future<Output = ()> + 'static) -> Flow {
        let task = Rc::new(Task {
            body: RefCell::new(Some(Box::pin(body))),
            running: Cell::new(false),
            again: Cell::new(false),
            canceled: Cell::new(false),
        });
        task.resume();
        Flow(Some(task))
    }

    /// Started, and neither done nor canceled.
    pub fn is_running(&self) -> bool {
        self.0.as_ref().is_some_and(|t| t.running.get() || t.body.borrow().is_some())
    }
}

impl Drop for Flow {
    fn drop(&mut self) {
        if let Some(task) = self.0.take() {
            task.canceled.set(true);
            let body = task.body.borrow_mut().take();
            drop(body); // its waits leave their Timer and Inbox
        }
    }
}

// ---- Timer ------------------------------------------------------------------

// A suspended wait with a deadline, which its Timer expires.
struct Timed {
    deadline: Instant,
    task: Weak<Task>,
    fired: Cell<bool>,
}

struct TimerState {
    now: Instant,
    timed: Vec<Rc<Timed>>,
    announced: Instant,
}

struct TimerInner {
    state: RefCell<TimerState>,
    wake: Option<Box<dyn Fn(Instant)>>,
}

/// Time for flows. The owning module calls tick() with each Tick's time, and
/// timestamped events move it too (Inbox::deliver); due waits resume then.
/// Time only moves forward. Ticks come once a second; for a wait that must
/// end sooner, give the Timer `wake`: it's called with each new earliest
/// deadline, for the owner to post WakeAt, and tick() it on the Wake.
/// Clones are the same timer.
#[derive(Clone)]
pub struct Timer(Rc<TimerInner>);

impl Default for Timer {
    fn default() -> Self {
        Timer::new()
    }
}

impl Timer {
    pub fn new() -> Self {
        Timer::build(None)
    }
    pub fn with_wake(wake: impl Fn(Instant) + 'static) -> Self {
        Timer::build(Some(Box::new(wake)))
    }
    fn build(wake: Option<Box<dyn Fn(Instant)>>) -> Self {
        Timer(Rc::new(TimerInner {
            state: RefCell::new(TimerState { now: Instant::ZERO, timed: Vec::new(), announced: Instant::ZERO }),
            wake,
        }))
    }

    pub fn now(&self) -> Instant {
        self.0.state.borrow().now
    }

    /// `.await` it: resumes on the first tick at or after the deadline, or
    /// at once if it has already passed.
    pub fn until(&self, deadline: Instant) -> Sleep {
        Sleep { timer: self.clone(), deadline, timed: None }
    }
    pub fn after(&self, delay: Duration) -> Sleep {
        self.until(self.now() + delay)
    }

    pub fn tick(&self, now: Instant) {
        {
            let mut st = self.0.state.borrow_mut();
            st.now = st.now.max(now);
        }
        // One at a time: expiring resumes a flow, which may start or cancel others.
        loop {
            let due = {
                let mut st = self.0.state.borrow_mut();
                let now = st.now;
                st.timed.iter().position(|t| t.deadline <= now).map(|i| st.timed.remove(i))
            };
            let Some(due) = due else { break };
            due.fired.set(true);
            if let Some(task) = due.task.upgrade() {
                task.resume();
            }
        }
        self.announce();
    }

    fn start(&self, deadline: Instant) -> Rc<Timed> {
        let timed = Rc::new(Timed { deadline, task: current(), fired: Cell::new(false) });
        self.0.state.borrow_mut().timed.push(timed.clone());
        self.announce();
        timed
    }

    fn stop(&self, timed: &Rc<Timed>) {
        self.0.state.borrow_mut().timed.retain(|t| !Rc::ptr_eq(t, timed));
    }

    // The earliest deadline to `wake`, if it's new.
    fn announce(&self) {
        let Some(wake) = &self.0.wake else { return };
        let earliest = {
            let mut st = self.0.state.borrow_mut();
            let Some(earliest) = st.timed.iter().map(|t| t.deadline).min() else { return };
            if earliest == st.announced {
                return;
            }
            st.announced = earliest;
            earliest
        };
        wake(earliest);
    }
}

/// Timer::until's future.
pub struct Sleep {
    timer: Timer,
    deadline: Instant,
    timed: Option<Rc<Timed>>,
}

impl Future for Sleep {
    type Output = ();
    fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<()> {
        let this = self.get_mut();
        if let Some(timed) = &this.timed {
            if !timed.fired.get() {
                return Poll::Pending;
            }
            this.timed = None;
            return Poll::Ready(());
        }
        if this.deadline <= this.timer.now() {
            return Poll::Ready(());
        }
        this.timed = Some(this.timer.start(this.deadline));
        Poll::Pending
    }
}

impl Drop for Sleep {
    fn drop(&mut self) {
        if let Some(timed) = self.timed.take() {
            self.timer.stop(&timed); // the flow was canceled while waiting
        }
    }
}

// ---- Inbox --------------------------------------------------------------------

struct Waiting<T> {
    filter: Filter<T>,
    task: Weak<Task>,
    serial: u32,
    value: RefCell<Option<T>>,
}

struct InboxState<T> {
    waiting: Vec<Rc<Waiting<T>>>,
    serial: u32,
}

struct InboxInner<T> {
    timer: Timer,
    state: RefCell<InboxState<T>>,
}

/// Hands each T to the flows waiting for one (each with its own filter and
/// optional deadline). The owning module calls deliver() from its handler
/// for T. A T nobody is waiting for is dropped. Clones are the same inbox.
pub struct Inbox<T>(Rc<InboxInner<T>>);

impl<T> Clone for Inbox<T> {
    fn clone(&self) -> Self {
        Inbox(self.0.clone())
    }
}

impl<T: Clone + 'static> Inbox<T> {
    pub fn new(timer: &Timer) -> Self {
        Inbox(Rc::new(InboxInner {
            timer: timer.clone(),
            state: RefCell::new(InboxState { waiting: Vec::new(), serial: 0 }),
        }))
    }

    /// `.await` the next T.
    pub fn next(&self) -> impl Future<Output = T> + use<T> {
        let wait = self.wait(Box::new(|_| true), Instant::MAX);
        async move { wait.await.expect("a wait without a deadline doesn't time out") }
    }
    /// `.await` the next T that passes `filter`.
    pub fn next_where<F: Fn(&T) -> bool + 'static>(&self, filter: F) -> impl Future<Output = T> + use<T, F> {
        let wait = self.wait(Box::new(filter), Instant::MAX);
        async move { wait.await.expect("a wait without a deadline doesn't time out") }
    }
    /// The same, or None once `deadline` has passed.
    pub fn next_by(&self, deadline: Instant) -> Wait<T> {
        self.wait(Box::new(|_| true), deadline)
    }
    pub fn next_where_by(&self, filter: impl Fn(&T) -> bool + 'static, deadline: Instant) -> Wait<T> {
        self.wait(Box::new(filter), deadline)
    }

    fn wait(&self, filter: Filter<T>, deadline: Instant) -> Wait<T> {
        Wait { inbox: self.clone(), filter: Some(filter), deadline, waiting: None, timed: None }
    }

    /// `at`, if given, is when it happened: the timer moves on to it first,
    /// so waits whose deadline had passed by then time out (and a flow that
    /// then waits again here can take this one).
    pub fn deliver(&self, value: &T, at: Option<Instant>) {
        if let Some(at) = at {
            self.0.timer.tick(at);
        }
        // Only those waiting now: a resumed flow that waits again gets the next one.
        let last = self.0.state.borrow().serial;
        loop {
            let found = {
                let mut st = self.0.state.borrow_mut();
                let i = st.waiting.iter().position(|w| w.serial <= last && (w.filter)(value));
                i.map(|i| st.waiting.remove(i))
            };
            let Some(w) = found else { break };
            *w.value.borrow_mut() = Some(value.clone());
            if let Some(task) = w.task.upgrade() {
                task.resume();
            }
        }
    }
}

/// An Inbox's future: the T, or None if the deadline passed first.
pub struct Wait<T> {
    inbox: Inbox<T>,
    filter: Option<Filter<T>>, // until it waits
    deadline: Instant,
    waiting: Option<Rc<Waiting<T>>>,
    timed: Option<Rc<Timed>>,
}

impl<T> Wait<T> {
    fn leave(&mut self) {
        if let Some(w) = self.waiting.take() {
            self.inbox.0.state.borrow_mut().waiting.retain(|x| !Rc::ptr_eq(x, &w));
        }
        if let Some(timed) = self.timed.take() {
            self.inbox.0.timer.stop(&timed);
        }
    }
}

impl<T> Future for Wait<T> {
    type Output = Option<T>;
    fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<Option<T>> {
        let this = self.get_mut();
        if let Some(w) = &this.waiting {
            let value = w.value.borrow_mut().take();
            if value.is_some() || this.timed.as_ref().is_some_and(|t| t.fired.get()) {
                this.leave();
                return Poll::Ready(value);
            }
            return Poll::Pending;
        }
        let timer = &this.inbox.0.timer;
        if this.deadline <= timer.now() {
            return Poll::Ready(None);
        }
        let waiting = {
            let mut st = this.inbox.0.state.borrow_mut();
            st.serial += 1;
            let w = Rc::new(Waiting {
                filter: this.filter.take().expect("polled again after it finished"),
                task: current(),
                serial: st.serial,
                value: RefCell::new(None),
            });
            st.waiting.push(w.clone());
            w
        };
        this.waiting = Some(waiting);
        if this.deadline != Instant::MAX {
            this.timed = Some(timer.start(this.deadline));
        }
        Poll::Pending
    }
}

impl<T> Drop for Wait<T> {
    fn drop(&mut self) {
        self.leave(); // the flow was canceled while waiting
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_flow_waits_times_out_and_is_canceled_by_dropping_it() {
        let timer = Timer::new();
        let inbox = Inbox::<u32>::new(&timer);
        let seen = Rc::new(RefCell::new(Vec::new()));
        let flow = Flow::start({
            let (timer, inbox, seen) = (timer.clone(), inbox.clone(), seen.clone());
            async move {
                loop {
                    let got = inbox.next_where_by(|v| v % 2 == 0, timer.now() + Duration::secs(5)).await;
                    seen.borrow_mut().push(got);
                }
            }
        });
        inbox.deliver(&1, None); // filtered out
        inbox.deliver(&2, None);
        timer.tick(Instant::from_boot(Duration::secs(5)));
        assert_eq!(*seen.borrow(), [Some(2), None]);
        drop(flow);
        inbox.deliver(&4, None);
        timer.tick(Instant::from_boot(Duration::secs(60)));
        assert_eq!(seen.borrow().len(), 2);
        assert!(inbox.0.state.borrow().waiting.is_empty() && timer.0.state.borrow().timed.is_empty());
    }
}
