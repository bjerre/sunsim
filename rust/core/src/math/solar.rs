//! The sun's times and position. Pure, like daylight.rs and sky.rs.

use std::f64::consts::PI;

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SunTimes {
    pub sunrise: f64, // hours after local midnight, in [0, 24)
    pub sunset: f64,
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SunPosition {
    pub azimuth_deg: f64,   // compass bearing, clockwise from North: 90 = East, 180 = South, 270 = West
    pub elevation_deg: f64, // above the horizon; negative = below
}

const DEG2RAD: f64 = PI / 180.0;
const RAD2DEG: f64 = 180.0 / PI;

/// Horizon (90 deg) plus ~50' of atmospheric refraction plus the sun's
/// apparent radius: the standard zenith angle used to define sunrise/sunset.
const ZENITH_DEG: f64 = 90.833;

fn normalize_degrees(deg: f64) -> f64 {
    let deg = deg % 360.0;
    if deg < 0.0 { deg + 360.0 } else { deg }
}

fn normalize_hours(hours: f64) -> f64 {
    let hours = hours % 24.0;
    if hours < 0.0 { hours + 24.0 } else { hours }
}

fn sun_event_local_hour(
    latitude_deg: f64,
    longitude_deg: f64,
    day_of_year: i32,
    utc_offset_hours: f64,
    is_sunrise: bool,
) -> f64 {
    let lng_hour = longitude_deg / 15.0;
    let base_hour = if is_sunrise { 6.0 } else { 18.0 };
    let t = day_of_year as f64 + ((base_hour - lng_hour) / 24.0);

    let m = (0.9856 * t) - 3.289;

    let l = normalize_degrees(m + (1.916 * (m * DEG2RAD).sin()) + (0.020 * (2.0 * m * DEG2RAD).sin()) + 282.634);

    let mut ra = normalize_degrees(RAD2DEG * (0.91764 * (l * DEG2RAD).tan()).atan());
    let l_quadrant = (l / 90.0).floor() * 90.0;
    let ra_quadrant = (ra / 90.0).floor() * 90.0;
    ra = (ra + (l_quadrant - ra_quadrant)) / 15.0; // hours

    let sin_dec = 0.39782 * (l * DEG2RAD).sin();
    let cos_dec = sin_dec.asin().cos();

    // Polar day/night: clamp instead of leaving an acos() domain error, so
    // this degenerates to a zero- or full-length window for the caller's
    // brightness curve rather than propagating NaN into bulb commands.
    let cos_h = (((ZENITH_DEG * DEG2RAD).cos() - (sin_dec * (latitude_deg * DEG2RAD).sin()))
        / (cos_dec * (latitude_deg * DEG2RAD).cos()))
    .clamp(-1.0, 1.0);

    let h_deg = if is_sunrise { 360.0 - RAD2DEG * cos_h.acos() } else { RAD2DEG * cos_h.acos() };
    let h = h_deg / 15.0; // hours

    let time_utc = h + ra - (0.06571 * t) - 6.622 - lng_hour;
    normalize_hours(time_utc + utc_offset_hours)
}

/// Local civil-time sunrise/sunset for a given latitude/longitude and day of
/// year, via the "Sunrise/Sunset Algorithm" from the Almanac for Computers,
/// 1990 (Nautical Almanac Office, US Naval Observatory).
///
/// - `latitude_deg`: degrees North (negative = South).
/// - `longitude_deg`: degrees East (negative = West).
/// - `day_of_year`: 1-366 (TimeReading::day_of_year()).
/// - `utc_offset_hours`: the local civil UTC offset in effect on that day,
///   including DST, positive = East of Greenwich (see the platform's Clock
///   for how it's derived).
///
/// At latitudes where the sun doesn't rise/set that day (inside a polar
/// circle in local winter/summer), the underlying angle is clamped rather
/// than left as a domain error, which degenerates to a same-valued
/// sunrise/sunset (zero-length or full-length day) rather than NaN.
pub fn compute_sun_times(latitude_deg: f64, longitude_deg: f64, day_of_year: i32, utc_offset_hours: f64) -> SunTimes {
    SunTimes {
        sunrise: sun_event_local_hour(latitude_deg, longitude_deg, day_of_year, utc_offset_hours, true),
        sunset: sun_event_local_hour(latitude_deg, longitude_deg, day_of_year, utc_offset_hours, false),
    }
}

/// Where the sun is in the sky at `local_time` (hours after local midnight)
/// on day_of_year, via NOAA's general solar position equations (fractional-
/// year Fourier fits for declination and the equation of time; accurate to
/// a fraction of a degree). Arguments as for compute_sun_times.
pub fn compute_sun_position(
    latitude_deg: f64,
    longitude_deg: f64,
    day_of_year: i32,
    local_time: f64,
    utc_offset_hours: f64,
) -> SunPosition {
    let utc_hour = local_time - utc_offset_hours;

    // Fractional year (radians).
    let g = 2.0 * PI / 365.0 * ((day_of_year - 1) as f64 + (utc_hour - 12.0) / 24.0);

    let eq_time_min = 229.18
        * (0.000075 + 0.001868 * g.cos()
            - 0.032077 * g.sin()
            - 0.014615 * (2.0 * g).cos()
            - 0.040849 * (2.0 * g).sin());
    let decl = 0.006918 - 0.399912 * g.cos() + 0.070257 * g.sin() - 0.006758 * (2.0 * g).cos()
        + 0.000907 * (2.0 * g).sin()
        - 0.002697 * (3.0 * g).cos()
        + 0.00148 * (3.0 * g).sin();

    // True solar time (minutes) -> hour angle (0 at solar noon, negative in the morning).
    let true_solar_min = utc_hour * 60.0 + eq_time_min + 4.0 * longitude_deg;
    let hour_angle = (true_solar_min / 4.0 - 180.0) * DEG2RAD;

    let lat = latitude_deg * DEG2RAD;
    let cos_zen = lat.sin() * decl.sin() + lat.cos() * decl.cos() * hour_angle.cos();
    let elevation = 90.0 - cos_zen.clamp(-1.0, 1.0).acos() * RAD2DEG;

    // Measured from South, positive toward West; +180 makes it a compass bearing.
    let from_south = hour_angle.sin().atan2(hour_angle.cos() * lat.sin() - decl.tan() * lat.cos());
    SunPosition { azimuth_deg: normalize_degrees(from_south * RAD2DEG + 180.0), elevation_deg: elevation }
}
