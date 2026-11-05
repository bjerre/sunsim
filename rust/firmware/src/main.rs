//! sunsim on the board, in Rust: the core, the clock and the Zigbee adapter
//! on the app task, as the C++ main/main.cpp runs them, with what's kept in
//! NVS (platform::SettingsStore on storage.rs) and the internet sync
//! (platform::InternetSync on internet.rs), and the serial console
//! (platform::Console on console.rs). Still to port from the C++ platform:
//! the display.

mod clock;
mod config;
mod console;
mod internet;
mod storage;
mod zigbee;

use std::cell::RefCell;
use std::rc::Rc;

use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::nvs::EspDefaultNvsPartition;
use ev::*;
use platform::{InternetSync, SettingsStore, StatusSlot, Syncer, WifiStore};
use sunsim_core::{CoreModules, wire_core};
use zigbee::{StackLock, ZigbeeAdapter};

const TAG: &str = "APP";

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();
    clock::set_time_zone(config::TZ);
    // NVS, for the settings and the Zigbee stack's network.
    let nvs = EspDefaultNvsPartition::take().expect("NVS");
    let peripherals = Peripherals::take().expect("the peripherals");
    let sysloop = EspSystemEventLoop::take().expect("the system event loop");

    // The bus and every module, wired as the firmware wires them: the core,
    // then the platform's.
    let mut bus = EventBus::new();
    let core = CoreModules::new(bus.post(), ADDRESS_OF, Vec::new());
    let storage = storage::Nvs::open(nvs.clone()).expect("the settings' NVS namespace");
    let settings = Rc::new(RefCell::new(SettingsStore::new(bus.post().clone(), storage, config::default_program())));
    let zigbee = Rc::new(RefCell::new(ZigbeeAdapter::new(bus.post().clone()).expect("the Zigbee adapter")));
    let board = clock::Board::start(bus.post()).expect("the clock's timers");
    let clock = Rc::new(RefCell::new(platform::Clock::new(bus.post().clone(), board, config::fast_day())));
    let net = internet::EspInternet::new(peripherals.modem, sysloop, nvs.clone()).expect("Wi-Fi");
    let status = Rc::new(RefCell::new(StatusSlot::default()));
    let program = config::default_program();

    wire_core(&mut bus, &core);
    // Settings before the radio: on FactoryReset the stored roles are erased
    // first, as the radio's reset reboots and never returns.
    bus.attach(&settings);
    bus.attach(&clock);
    bus.attach(&zigbee);
    bus.attach(&status);

    settings.borrow_mut().load_all(); // queued until the app task starts processing

    // The boot sync, before the radio starts, so Wi-Fi has it to itself.
    let syncer = Syncer::new(bus.post().clone(), net, program.latitude_deg, program.longitude_deg);
    let internet = Rc::new(RefCell::new(InternetSync::start(syncer, |task| {
        std::thread::Builder::new()
            .name("internet".into())
            .stack_size(12 * 1024) // TLS: the C++ firmware's 8 KB, plus room for Rust's frames
            .spawn(task)
            .expect("the internet task starts");
    })));
    bus.attach(&internet);

    zigbee.borrow().start();

    let wifi = WifiStore::new(storage::Nvs::open(nvs).expect("the Wi-Fi networks' NVS namespace"), Some(config::wifi_fallback()));
    let day_seconds = config::DEBUG_DAY_SECONDS.parse().expect("SUNSIM_DEBUG_DAY_SECONDS is a number");
    console::start(bus.post().clone(), status.borrow().clone(), wifi, day_seconds).expect("the console");

    // The app task (this one): the bus's single consumer. Handlers may call
    // into the Zigbee stack, which needs its lock - and the lock only exists
    // once the stack is initialized.
    zigbee.borrow().wait_until_ready();
    log::info!(target: TAG, "Processing events");
    loop {
        bus.wait();
        let _stack = StackLock::acquire();
        bus.process();
    }
}
