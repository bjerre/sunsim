//! The functions the page calls, as the Emscripten build of the C++
//! simulator exported them (web/wasm.d.ts): the one office, on the page's
//! one thread.
#![allow(unsafe_code)] // no_mangle: names JavaScript can find

use std::cell::RefCell;

use crate::Office;

thread_local! {
    static OFFICE: RefCell<Option<Office>> = const { RefCell::new(None) };
    // The last state, NUL-terminated, for sunsim_state's pointer to stay valid until the next.
    static STATE: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

fn with<T>(f: impl FnOnce(&mut Office) -> T) -> T {
    OFFICE.with_borrow_mut(|office| f(office.get_or_insert_with(Office::new)))
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_init() {
    with(|_| ());
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_advance(seconds: i32) {
    with(|o| o.advance(seconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_set_minute(minute: i32) {
    with(|o| o.set_minute(minute));
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_set_location(latitude: f64, longitude: f64) -> i32 {
    with(|o| o.set_location(latitude, longitude)) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_set_clock(
    year: i32,
    month: i32,
    day: i32,
    hour: i32,
    minute: i32,
    utc_offset_minutes: i32,
) -> i32 {
    with(|o| o.set_clock(year, month, day, hour, minute, utc_offset_minutes)) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_press(kind: i32) {
    with(|o| o.press(kind));
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_weather(precip: i32, cloud: i32) {
    with(|o| o.weather(precip, cloud));
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_alert(level: i32) {
    with(|o| o.alert(level));
}

/// The state as JSON: a NUL-terminated UTF-8 string, valid until the next call.
#[unsafe(no_mangle)]
pub extern "C" fn sunsim_state() -> *const u8 {
    let json = with(|o| o.state_json());
    STATE.with_borrow_mut(|state| {
        state.clear();
        state.extend_from_slice(json.as_bytes());
        state.push(0);
        state.as_ptr()
    })
}
