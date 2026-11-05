//! The board's clocks, for the platform's Clock (platform::clock): the boot
//! clock (the esp_timer), the wall clock (set by SNTP) and its time zone,
//! a 1 Hz Heartbeat and a one-shot timer for Wakes.

use std::time::{SystemTime, UNIX_EPOCH};

use esp_idf_svc::sys::{EspError, esp_timer_get_time, localtime_r, time_t, tm, tzset};
use esp_idf_svc::timer::{EspTaskTimerService, EspTimer};
use ev::*;
use platform::{ClockHardware, zone};

/// Steady time since boot: the esp_timer.
pub fn now() -> Instant {
    // SAFETY: a read of the running esp_timer.
    Instant::from_boot(Duration::micros(unsafe { esp_timer_get_time() }))
}

/// The local time zone, from a POSIX TZ string. At boot, before any other
/// task reads the environment.
pub fn set_time_zone(tz: &str) {
    // SAFETY: nothing else runs yet that reads the environment.
    unsafe {
        std::env::set_var("TZ", tz);
        tzset();
    }
}

/// The wall clock, UTC: 1970 until it's set.
pub fn wall() -> SysSeconds {
    SysSeconds(SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64))
}

/// The local UTC offset at `at`, from the time zone (set_time_zone()).
pub fn utc_offset(at: SysSeconds) -> Duration {
    let t: time_t = at.0;
    // SAFETY: localtime_r fills in `local`, a C struct of plain fields.
    let local = unsafe {
        let mut local: tm = core::mem::zeroed();
        localtime_r(&t, &mut local);
        local
    };
    let field = |v: i32| v as u32;
    zone::utc_offset(
        at,
        (
            local.tm_year + 1900,
            field(local.tm_mon + 1),
            field(local.tm_mday),
            field(local.tm_hour),
            field(local.tm_min),
            field(local.tm_sec),
        ),
    )
}

pub struct Board {
    _heartbeat: EspTimer<'static>,
    wake: EspTimer<'static>,
}

impl Board {
    /// The Heartbeat starts at once, once a second.
    pub fn start(post: &Post) -> Result<Self, EspError> {
        let timers = EspTaskTimerService::new()?;
        let beats = post.clone();
        let heartbeat = timers.timer(move || beats.send(Heartbeat { at: now() }))?;
        heartbeat.every(std::time::Duration::from_secs(1))?;
        let wakes = post.clone();
        let wake = timers.timer(move || wakes.send(Wake { at: now() }))?;
        Ok(Board { _heartbeat: heartbeat, wake })
    }
}

impl ClockHardware for Board {
    fn now(&self) -> Instant {
        now()
    }

    fn wall(&self) -> SysSeconds {
        wall()
    }

    fn utc_offset(&self, at: SysSeconds) -> Duration {
        utc_offset(at)
    }

    fn arm_wake(&mut self, at: Option<Instant>) {
        let _ = self.wake.cancel(); // fails, harmlessly, if it isn't running
        if let Some(at) = at {
            let us = (at - now()).as_micros().max(1) as u64;
            if let Err(e) = self.wake.after(std::time::Duration::from_micros(us)) {
                log::error!(target: "CLOCK", "No wake: {e}");
            }
        }
    }
}
