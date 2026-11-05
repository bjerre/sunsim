//! The local UTC offset.

use ev::{Duration, SysSeconds, days_from_civil};

/// The local civil UTC offset (DST included) at `at`, from the local time's
/// calendar fields there (localtime_r's: year, month 1-12, day, hour,
/// minute, second): those fields read as if they were UTC, less the instant
/// itself, to the minute. (No portable tm_gmtoff; mktime() on gmtime()'s
/// fields would be off by the DST hour, as gmtime() sets tm_isdst = 0.)
pub fn utc_offset(at: SysSeconds, local: (i32, u32, u32, u32, u32, u32)) -> Duration {
    let (y, mo, d, h, mi, s) = local;
    let as_if_utc = days_from_civil(y, mo, d) * 86_400 + (h * 3600 + mi * 60 + s) as i64;
    let secs = as_if_utc - at.0;
    Duration::minutes((secs as f64 / 60.0).round() as i64)
}
