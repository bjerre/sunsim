//! Day length and the next sunrise/sunset, for the status display/console.
//! Pure, like daylight.rs and solar.rs.

use ev::{DayInfo, TimeReading};

use super::solar::compute_sun_times;

pub fn compute_day_info(time: &TimeReading, latitude_deg: f64, longitude_deg: f64) -> DayInfo {
    const DAY: f64 = 24.0;

    let now = time.since_midnight().as_hours_f64();
    let offset = time.utc_offset.as_hours_f64();
    let day_of_year = time.day_of_year();
    let today = compute_sun_times(latitude_deg, longitude_deg, day_of_year, offset);

    let mut day_length = today.sunset - today.sunrise;
    if day_length < 0.0 {
        day_length += DAY; // window spills past midnight (near-polar summer)
    }

    let mut info = DayInfo {
        sunrise: today.sunrise,
        sunset: today.sunset,
        day_length,
        next_is_sunrise: true,
        next_event: today.sunrise,
        until_next: today.sunrise - now,
    };

    if now < today.sunrise {
        return info;
    }
    if now < today.sunset {
        info.next_is_sunrise = false;
        info.next_event = today.sunset;
        info.until_next = today.sunset - now;
        return info;
    }

    // After sunset: count to tomorrow's sunrise. Day 366 wraps to 1; the
    // sunrise difference across New Year is a fraction of a minute.
    let tomorrow = if day_of_year >= 366 { 1 } else { day_of_year + 1 };
    let next = compute_sun_times(latitude_deg, longitude_deg, tomorrow, offset);
    info.next_event = next.sunrise;
    info.until_next = DAY - now + next.sunrise;
    info
}
