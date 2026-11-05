//! The platform modules' logic, without ESP-IDF: what the C++
//! sunsim::platform does besides calling the IDF, here so the host tests
//! cover it. Each module takes its hardware edges as a trait, which the
//! firmware (../firmware) implements with esp-idf-svc and the tests with
//! fakes.

pub mod clock;
pub mod console;
pub mod internet;
pub mod settings_store;
pub mod storage;
pub mod wifi;
pub mod zone;

pub use clock::{Clock, ClockHardware, FastDayClock};
pub use console::{Console, ConsoleBoard, StatusSlot};
pub use internet::{Internet, InternetSync, Syncer};
pub use settings_store::SettingsStore;
pub use storage::Storage;
pub use wifi::{WifiNetwork, WifiNetworks, WifiSaveResult, WifiStore};
