//! The functions the page calls, as the Emscripten build of the C++
//! simulator exported them (web/wasm.d.ts): the one office, on the page's
//! one thread.
#![allow(unsafe_code)] // no_mangle: names JavaScript can find

use std::alloc::{Layout, alloc, dealloc};
use std::cell::RefCell;
use std::ffi::{CStr, c_char};
use std::sync::Arc;

use ev::ProfileBuilder;

use crate::Office;

thread_local! {
    static OFFICE: RefCell<Option<Office>> = const { RefCell::new(None) };
    // The profile the page builds before the office starts, if it brings one.
    static PROFILE: RefCell<ProfileBuilder> = RefCell::new(ProfileBuilder::new());
    // The last state, NUL-terminated, for sunsim_state's pointer to stay valid until the next.
    static STATE: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

fn with<T>(f: impl FnOnce(&mut Office) -> T) -> T {
    OFFICE.with_borrow_mut(|office| {
        f(office.get_or_insert_with(|| match PROFILE.with_borrow(|b| b.installation().cloned()) {
            Some(installation) => Office::with_installation(Arc::new(installation)),
            None => Office::new(),
        }))
    })
}

// A profile step, refused (0) once the office has started.
fn profile(f: impl FnOnce(&mut ProfileBuilder) -> bool) -> i32 {
    if OFFICE.with_borrow(Option::is_some) {
        return 0;
    }
    PROFILE.with_borrow_mut(f) as i32
}

// ---- memory, for the page's strings ----------------------------------------

// Each block starts with its size, so free needs only the pointer.
const HEADER: usize = 8;

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_malloc(size: usize) -> *mut u8 {
    let Ok(layout) = Layout::from_size_align(size + HEADER, HEADER) else { return std::ptr::null_mut() };
    // SAFETY: the layout isn't zero-sized; the header fits the block.
    unsafe {
        let block = alloc(layout);
        if block.is_null() {
            return block;
        }
        block.cast::<usize>().write(size);
        block.add(HEADER)
    }
}

/// # Safety
/// `pointer` came from sunsim_malloc and wasn't freed, or is null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sunsim_free(pointer: *mut u8) {
    if pointer.is_null() {
        return;
    }
    // SAFETY: as promised, the block and its header are sunsim_malloc's.
    unsafe {
        let block = pointer.sub(HEADER);
        let size = block.cast::<usize>().read();
        dealloc(block, Layout::from_size_align_unchecked(size + HEADER, HEADER));
    }
}

// ---- the installation profile, before init ----------------------------------

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_profile_begin(zone_count: i32) -> i32 {
    profile(|b| b.begin(zone_count))
}

/// # Safety
/// `name` is a NUL-terminated string, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sunsim_profile_zone(
    index: i32,
    name: *const c_char,
    start_minute: i32,
    end_minute: i32,
) -> i32 {
    if name.is_null() {
        return 0;
    }
    // SAFETY: as promised.
    let Ok(name) = (unsafe { CStr::from_ptr(name) }).to_str() else { return 0 };
    profile(|b| b.set_zone(index, name, start_minute, end_minute))
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_profile_member(zone: i32, role: i32, look: i32, peak_percent: i32) -> i32 {
    profile(|b| b.add_member(zone, role, look, peak_percent))
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_profile_button(zone: i32, role: i32) -> i32 {
    profile(|b| b.set_button(zone, role))
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_profile_weather(zone: i32, role: i32, effect: i32) -> i32 {
    profile(|b| b.add_weather_role(zone, role, effect))
}

#[unsafe(no_mangle)]
pub extern "C" fn sunsim_profile_finish() -> i32 {
    profile(ProfileBuilder::finish)
}

// ---- the office ---------------------------------------------------------------

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
