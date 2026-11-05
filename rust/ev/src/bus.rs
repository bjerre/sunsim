//! The one event queue. Producers on any task post; a single consumer (the
//! app task on the target, the test on the host) runs process(), which
//! calls every listener for each queued event in order. Listeners may post
//! more events; those are handled on a following process() pass.

use std::cell::RefCell;
use std::mem;
use std::rc::Rc;
use std::sync::{Arc, Condvar, Mutex};

use crate::events::{Event, Payload, Post};

/// A module on the bus: it gets every event, and handles the ones it's
/// for (a `match` on the event, the rest ignored).
pub trait Module {
    fn on(&mut self, e: &Event);
}

type Listener = Box<dyn FnMut(&Event)>;

#[derive(Default)]
struct Queue {
    pending: Mutex<Vec<Event>>,
    ready: Condvar,
}

/// process() takes everything queued so far under the lock, then delivers it
/// without holding it, so posting never waits on a listener. Listeners are
/// added while wiring, before anything is processed, and never removed.
pub struct EventBus {
    queue: Arc<Queue>,
    listeners: Vec<Listener>,
    batch: Vec<Event>, // the consumer's, while it delivers
    post: Post,
}

impl Default for EventBus {
    fn default() -> Self {
        EventBus::new()
    }
}

impl EventBus {
    pub fn new() -> Self {
        let queue = Arc::new(Queue::default());
        let posted = queue.clone();
        let post = Post::new(move |e| {
            log::debug!(target: "EV", "{}", e.name()); // the full event trace, at DEBUG
            posted.pending.lock().unwrap().push(e);
            posted.ready.notify_one();
        });
        EventBus { queue, listeners: Vec::new(), batch: Vec::new(), post }
    }

    /// Listen for payload T: `bus.on::<Tick>(|t| ...)`.
    pub fn on<T: Payload>(&mut self, mut listener: impl FnMut(&T) + 'static) {
        self.listeners.push(Box::new(move |e| {
            if let Some(p) = T::of(e) {
                listener(p);
            }
        }));
    }

    /// Listen for every event.
    pub fn on_any(&mut self, listener: impl FnMut(&Event) + 'static) {
        self.listeners.push(Box::new(listener));
    }

    /// Each module gets every event, after the listeners added before it.
    /// Listeners for an event run in the order they were added; what they
    /// post is delivered on a later pass, so that order only matters for
    /// effects outside the bus (FactoryReset: erase, then reboot).
    pub fn attach<M: Module + 'static>(&mut self, module: &Rc<RefCell<M>>) {
        let module = module.clone();
        self.listeners.push(Box::new(move |e| module.borrow_mut().on(e)));
    }

    /// Any task.
    pub fn post(&self) -> &Post {
        &self.post
    }

    /// The consumer: block until something is queued.
    pub fn wait(&self) {
        let pending = self.queue.pending.lock().unwrap();
        let _unused = self.queue.ready.wait_while(pending, |p| p.is_empty()).unwrap();
    }

    /// The consumer: deliver everything queued so far; false if there was nothing.
    pub fn process(&mut self) -> bool {
        // Both keep their capacity: no allocating once warmed up.
        mem::swap(&mut *self.queue.pending.lock().unwrap(), &mut self.batch);
        for e in &self.batch {
            for listener in &mut self.listeners {
                listener(e);
            }
        }
        let any = !self.batch.is_empty();
        self.batch.clear();
        any
    }

    /// Process until no events remain, including ones posted while processing.
    pub fn drain(&mut self) {
        while self.process() {}
    }
}
