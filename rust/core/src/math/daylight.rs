//! What each look shows. See DESIGN.md for the math.

use std::f64::consts::PI;

use ev::{Color, Look, Mireds, ProgramConfig, TimeReading, WorkCurve, Xy};

use super::solar::{compute_sun_position, compute_sun_times};

/// CIE 1931 xy of a blackbody at `kelvin` (Kim et al. cubic spline
/// approximation of the Planckian locus, valid 1667-25000K): for colors
/// below most bulbs' ~2700K color-temperature floor.
pub fn planckian_xy(kelvin: f64) -> Xy {
    let t = kelvin.clamp(1667.0, 25000.0);
    let t2 = t * t;
    let t3 = t2 * t;
    let x = if t <= 4000.0 {
        -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
    } else {
        -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390
    };
    let x2 = x * x;
    let x3 = x2 * x;
    let y = if t <= 2222.0 {
        -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
    } else if t <= 4000.0 {
        -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
    } else {
        3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483
    };
    Xy { x: x as f32, y: y as f32 }
}

/// What a look sees: the settings, the time, and where the light is.
#[derive(Clone, Copy, Debug)]
pub struct LookContext<'a> {
    pub config: &'a ProgramConfig,
    /// Carries the local civil UTC offset in effect (DST included; see the
    /// platform's Clock for how it's derived), which the sun's position needs.
    pub time: TimeReading,
    /// Along its group, as the group is lettered: +1 the first (for the wall:
    /// the east end), 0 the middle, -1 the last.
    pub position: f64,
    pub cloud: f64, // the cloud cover now, 0 (clear) to 1 (overcast)
}

/// What a look shows while it's on: how bright, as a fraction of the light's
/// own range (0: its minimum level, 1: its peak), and in what color.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Glow {
    pub brightness: f64,
    pub color: Color,
}

/// What `look` shows (None: off): ungated, as if the light may be on; the
/// gate decides whether it is.
pub fn show(look: &Look, ctx: &LookContext) -> Option<Glow> {
    match look {
        Look::SunCurve => sun_curve(ctx),
        Look::WorkCurve(curve) => work_curve(curve, ctx),
        // A plug has no dimming or color; the zone's gate alone switches it.
        Look::OnOffOnly => Some(Glow { brightness: 1.0, color: Color::default() }),
    }
}

/// The sun's curve for the configured location: warm and dim at the real
/// sunrise and sunset, brightest and coolest at solar noon, off at night;
/// dimming away from the sun along the wall, amber while the sun is low,
/// flattened by cloud. See solar.rs for the sun's times and position.
fn sun_curve(ctx: &LookContext) -> Option<Glow> {
    let c = ctx.config;
    let time = &ctx.time;

    // Clouds make the light diffuse: dimmer, cooler, no sun direction and
    // no golden glow, each in proportion to the cover.
    let overcast = if c.clouds_enabled { ctx.cloud.clamp(0.0, 1.0) } else { 0.0 };

    let now = time.since_midnight().as_hours_f64();
    let utc_offset = time.utc_offset.as_hours_f64();
    let day_of_year = time.day_of_year();
    let sun = compute_sun_times(c.latitude_deg, c.longitude_deg, day_of_year, utc_offset);

    let sunrise = sun.sunrise;
    let mut sunset = sun.sunset;
    if sunset <= sunrise {
        // The window spills past local midnight (near-polar summer): unwrap
        // it onto a single increasing scale rather than treating every hour
        // as "outside" it.
        sunset += 24.0;
    }
    let mut hour_adj = now;
    if hour_adj < sunrise {
        hour_adj += 24.0;
    }

    if hour_adj < sunrise || hour_adj > sunset {
        return None; // off
    }

    let day_progress = (hour_adj - sunrise) / (sunset - sunrise);
    let brightness = (PI * day_progress).sin(); // 0 at edges, 1 at solar noon

    // Sun direction: sin(azimuth) is the sun's east-west position (+1 due
    // East, 0 due South, -1 due West). `away` is how far along the wall the
    // bulb is from it: 0 under the sun, 1 at the opposite end. Bulbs further
    // away dim, by up to sun_shift_percent.
    let pos = compute_sun_position(c.latitude_deg, c.longitude_deg, day_of_year, now, utc_offset);
    let sun_x = (pos.azimuth_deg * PI / 180.0).sin();
    let away = (ctx.position - sun_x).abs() / 2.0;
    let direction = 1.0 - (c.sun_shift_percent as f64 / 100.0) * away * (1.0 - overcast);
    let sky = 1.0 - (c.cloud_dim_percent as f64 / 100.0) * overcast;

    let (warm, cool) = (c.warm_mireds as f64, c.cool_mireds as f64);
    let sun_mireds = warm - brightness * (warm - cool);
    let mireds = (sun_mireds + (cool - sun_mireds) * (c.cloud_cool_percent as f64 / 100.0) * overcast).round() as u16;

    let mut glow = Glow { brightness: direction * sky * brightness, color: Mireds(mireds).into() };

    // The amber glow follows the sun: bulbs further away see it as if it
    // were up to amber_sweep_deg higher, so they turn amber later and less.
    let amber_elevation = pos.elevation_deg + c.amber_sweep_deg as f64 * away;
    if c.amber_enabled && c.golden_elevation_deg > 0.0 && amber_elevation < c.golden_elevation_deg {
        // 0 at golden_elevation_deg, 1 at (or below) the horizon.
        let golden = ((c.golden_elevation_deg - amber_elevation) / c.golden_elevation_deg).min(1.0) * (1.0 - overcast); // no glow under a gray sky
        let amber_mireds = 1e6 / c.amber_kelvin;
        let m = mireds as f64 + golden * (amber_mireds - mireds as f64);
        glow.color = planckian_xy(1e6 / m).into();
    }
    Some(glow)
}

/// Cool to warm across the curve's hours, at full brightness.
fn work_curve(curve: &WorkCurve, ctx: &LookContext) -> Option<Glow> {
    let hours = curve.hours.value(ctx.config);
    let cool = curve.cool.value(ctx.config) as f64;
    let warm = curve.warm.value(ctx.config) as f64;

    // How far through the hours, 0 to 1.
    let progress = ((ctx.time.since_midnight() - hours.start).as_secs_f64() / (hours.end - hours.start).as_secs_f64())
        .clamp(0.0, 1.0);
    let warmth = progress * progress; // ease-in: stays cool through the morning
    let mireds = (cool + warmth * (warm - cool)).round() as u16;

    Some(Glow { brightness: 1.0, color: Mireds(mireds).into() })
}
