//! sunsim in the browser: the core on a simulated Zigbee network in virtual
//! time, with a wall clock and a location the page sets, as the Svelte
//! simulator (web/) drives it. `Office` is the simulation, host-tested;
//! exports.rs gives it to JavaScript as the functions the page calls, and
//! js/sunsim_web_simulator.js loads it as the module the page expects.

mod exports;
mod office;

pub use office::Office;
