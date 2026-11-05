//! The serial console on the board: the C6's USB-Serial-JTAG port (the
//! cable used for flashing), read a key at a time, echoed, a line at a time
//! to platform::Console, whose commands and replies are all there. Its
//! prompt and its words for a failed command are esp_console's, so
//! `./sunsim <command>` works as with the C++ firmware.

use std::io::Write;

use esp_idf_svc::sys::{
    EspError, esp, esp_restart, usb_serial_jtag_driver_config_t, usb_serial_jtag_driver_install,
    usb_serial_jtag_read_bytes, usb_serial_jtag_vfs_use_driver,
};
use ev::*;
use platform::{Console, ConsoleBoard, StatusSlot, WifiStore};

use crate::{clock, storage};

const BACKSPACE: u8 = 0x08;
const DELETE: u8 = 0x7F;

struct Board;

impl ConsoleBoard for Board {
    fn now(&self) -> Instant {
        clock::now()
    }
    fn restart(&mut self) {
        // SAFETY: a reboot; nothing after it runs.
        unsafe { esp_restart() }
    }
}

/// Starts the console on its own task, reading the port's driver (and
/// writing through it, logs and all, so they don't trip over each other).
pub fn start(post: Post, status: StatusSlot, wifi: WifiStore<storage::Nvs>, day_seconds: i64) -> Result<(), EspError> {
    let mut config = usb_serial_jtag_driver_config_t { tx_buffer_size: 1024, rx_buffer_size: 256 };
    // SAFETY: installs the driver once, before anything reads the port.
    unsafe {
        esp!(usb_serial_jtag_driver_install(&mut config))?;
        usb_serial_jtag_vfs_use_driver();
    }
    let mut console = Console::new(post, status, wifi, Board, day_seconds);
    std::thread::Builder::new()
        .name("console".into())
        .stack_size(8 * 1024) // commands format replies and copy snapshots
        .spawn(move || {
            loop {
                print!("{}", Console::<storage::Nvs, Board>::PROMPT);
                let line = read_line();
                let reply = console.line(&line);
                print!("{reply}");
            }
        })
        .expect("the console task starts");
    Ok(())
}

fn key() -> u8 {
    let mut byte = 0u8;
    // SAFETY: reads one byte into `byte`, blocking until there is one.
    while unsafe { usb_serial_jtag_read_bytes((&raw mut byte).cast(), 1, u32::MAX) } != 1 {}
    byte
}

// A typed line, echoed as it's typed: Enter ends it, Backspace takes the
// last character back.
fn read_line() -> String {
    let mut line = Vec::new();
    let mut out = std::io::stdout();
    loop {
        let _ = out.flush();
        match key() {
            b'\r' | b'\n' if line.is_empty() => {
                // A line ending's second half, or Enter on its own: a fresh prompt.
                println!();
                return String::new();
            }
            b'\r' | b'\n' => {
                println!();
                return String::from_utf8_lossy(&line).into_owned();
            }
            BACKSPACE | DELETE => {
                if line.pop().is_some() {
                    print!("\x08 \x08");
                }
            }
            c if c >= b' ' => {
                line.push(c);
                let _ = out.write_all(&[c]);
            }
            _ => {} // other control characters
        }
    }
}
