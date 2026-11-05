//! The forecast's cloud cover. Pure, like daylight.rs and solar.rs.

use ev::{CloudCover, TimeReading};

/// The forecast's cloud cover at `time`, 0 (clear) to 1 (overcast),
/// interpolated between its hours; None without data for then. Fast-day
/// mode's simulated day reads the forecast's first day, so the sped-up day
/// shows today's sky.
pub fn cloud_cover(forecast: &CloudCover, time: &TimeReading) -> Option<f64> {
    let day = forecast.day?;
    if !time.valid {
        return None;
    }
    // Hours into the forecast; fast-day's simulated day reads its first day.
    let into = if time.simulated { time.since_midnight() } else { time.local - day };
    let hour = into.as_hours_f64();
    if hour < 0.0 || hour > CloudCover::HOURS as f64 - 1.0 {
        return None;
    }

    let i = hour as usize;
    let here = forecast.percent[i];
    let next = if i + 1 < CloudCover::HOURS { forecast.percent[i + 1] } else { here };
    const NO_DATA: u8 = CloudCover::NO_DATA;
    match (here, next) {
        (NO_DATA, NO_DATA) => None,
        (NO_DATA, v) | (v, NO_DATA) => Some(v as f64 / 100.0),
        _ => {
            let t = hour - i as f64;
            Some((here as f64 + t * (next as f64 - here as f64)) / 100.0)
        }
    }
}
