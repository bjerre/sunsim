//! sunsim on simulated hardware: virtual time (scheduler), the board's
//! clock on it (sim_clock), and a Zigbee network of virtual bulbs and
//! buttons (sim_network), which stand in for the platform modules. World
//! puts them together with the core, as the firmware's app does with the
//! board's. Pure Rust, like the core.

pub mod scheduler;
pub mod sim_clock;
pub mod sim_network;

use std::cell::RefCell;
use std::rc::Rc;

pub use scheduler::Scheduler;
pub use sim_clock::SimClock;
pub use sim_network::{Bulb, Counts, Device, SimNetwork};

use ev::{Addresses, Duration, EventBus, Instant, TimeReading};
use sunsim_core::{CoreModules, LightGroup, wire_core};

/// The core, wired as the firmware wires it, with SimClock and SimNetwork
/// for the board's clock and radio. Add the devices (network.add()), then
/// start() and run (run_for()).
pub struct World {
    pub bus: EventBus,
    pub scheduler: Scheduler,
    pub core: CoreModules,
    pub clock: Rc<RefCell<SimClock>>,
    pub network: Rc<RefCell<SimNetwork>>,
}

impl World {
    pub fn new(start: TimeReading, addresses: Addresses, groups: Vec<LightGroup>) -> Self {
        let mut bus = EventBus::new();
        let scheduler = Scheduler::new();
        let core = CoreModules::new(bus.post(), addresses, groups);
        let clock = Rc::new(RefCell::new(SimClock::new(bus.post().clone(), scheduler.clone(), start)));
        let network = Rc::new(RefCell::new(SimNetwork::new(scheduler.clone())));
        wire_core(&mut bus, &core);
        bus.attach(&clock);
        bus.attach(&network);
        World { bus, scheduler, core, clock, network }
    }

    pub fn start(&mut self) {
        self.network.borrow_mut().start();
        self.clock.borrow_mut().start();
        self.run_for(Duration::ZERO); // what starting sets off
    }

    pub fn run_for(&mut self, duration: Duration) {
        self.scheduler.run_for(&mut self.bus, duration);
    }

    pub fn now(&self) -> Instant {
        self.scheduler.now()
    }
}
