//! All of the pure application logic. Platform code (radio, storage, clock,
//! console, display) talks to it only through events on the bus.

pub mod device_registry;
pub mod gate;
pub mod light_output;
pub mod math;
pub mod mode_controller;
pub mod overlays;
pub mod programs;
pub mod send_queue;
pub mod sky_model;
pub mod status_model;

use std::cell::RefCell;
use std::rc::Rc;

pub use device_registry::DeviceRegistry;
pub use gate::openness;
pub use light_output::{LightOutput, commands_for};
pub use math::day_info::compute_day_info;
pub use math::daylight::{Glow, LookContext, planckian_xy, show};
pub use math::sky::cloud_cover;
pub use math::solar::{SunPosition, SunTimes, compute_sun_position, compute_sun_times};
pub use mode_controller::ModeController;
pub use overlays::{Overlays, Pulse, Shade, weather_pulse};
pub use programs::ProgramHost;
pub use send_queue::{LightGroup, SendQueue};
pub use sky_model::SkyModel;
pub use status_model::StatusModel;

use ev::{Addresses, EventBus, Post};

/// All of the pure application logic, each module shared with the bus it's
/// attached to (wire_core).
pub struct CoreModules {
    pub mode: Rc<RefCell<ModeController>>,
    pub registry: Rc<RefCell<DeviceRegistry>>,
    pub sky: Rc<RefCell<SkyModel>>,
    pub programs: Rc<RefCell<ProgramHost>>,
    pub overlays: Rc<RefCell<Overlays>>,
    pub output: Rc<RefCell<LightOutput>>,
    pub sends: Rc<RefCell<SendQueue>>,
    pub status: Rc<RefCell<StatusModel>>,
}

impl CoreModules {
    /// `groups`: the Zigbee groups the bulbs are in, for SendQueue.
    pub fn new(post: &Post, addresses: Addresses, groups: Vec<LightGroup>) -> Self {
        CoreModules {
            mode: Rc::new(RefCell::new(ModeController::new(post.clone()))),
            registry: Rc::new(RefCell::new(DeviceRegistry::new(post.clone(), addresses))),
            sky: Rc::new(RefCell::new(SkyModel::new(post.clone()))),
            programs: Rc::new(RefCell::new(ProgramHost::new(post))),
            overlays: Rc::new(RefCell::new(Overlays::new(post.clone()))),
            output: Rc::new(RefCell::new(LightOutput::new(post.clone()))),
            sends: Rc::new(RefCell::new(SendQueue::new(post.clone(), groups))),
            status: Rc::new(RefCell::new(StatusModel::new(post.clone()))),
        }
    }
}

/// Attaches every core module to the bus, in the firmware's order. Platform
/// modules are attached separately; tests attach only these.
pub fn wire_core(bus: &mut EventBus, m: &CoreModules) {
    bus.attach(&m.mode);
    bus.attach(&m.registry);
    bus.attach(&m.sky);
    bus.attach(&m.programs);
    bus.attach(&m.overlays);
    bus.attach(&m.output);
    bus.attach(&m.sends);
    bus.attach(&m.status);
}
