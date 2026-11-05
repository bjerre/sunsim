//! The event vocabulary shared by every layer: value types, every event,
//! flows (async, on the app task), the EventBus, the settings table, what a
//! radio is, and the room (installation.rs, in topology.rs's words).
//! Everything is re-exported at the top: `ev::Tick`, `ev::ROLES`.

pub mod bus;
pub mod events;
pub mod flow;
pub mod installation;
pub mod radio;
pub mod settings;
pub mod time;
pub mod topology;
pub mod types;

pub use bus::*;
pub use events::*;
pub use flow::*;
pub use installation::*;
pub use radio::*;
pub use settings::{SETTINGS, Setting, find_setting, setting_keys};
pub use time::*;
pub use topology::*;
pub use types::*;
