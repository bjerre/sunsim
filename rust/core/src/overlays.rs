//! The weather lights: pulses over the program's targets.

use std::sync::Arc;

use ev::*;

/// A saturated color: hue in degrees, saturation 0-254 (Zigbee's).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Shade {
    pub hue_deg: u16,
    pub saturation: u8,
}

/// A light breathing between bright and dim in one color, like red alert.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Pulse {
    pub shade: Shade,
    pub fade_seconds: u8, // each fade, bright to dim or back: a cycle is twice this
    pub strength: f64,    // the top of the pulse, as a fraction of the light's peak
    pub effect: WeatherEffect,
}

// Quick, for handing a light back to its program.
const RELEASE_TRANSITION: Transition = Transition::secs(2);
const COLOR_TRANSITION: Transition = Transition::tenths(2);

// The bottom of a pulse, as a fraction of its top.
const DIM_FRACTION: f64 = 0.2;
// Rain or snow on a light the program has off: a glow, not the full pulse.
const UNLIT_STRENGTH: f64 = 0.35;

// Advisory yellow, watch orange, warning red; faster as it gets worse.
const fn alert_pulse(level: AlertLevel) -> Pulse {
    let (hue_deg, fade_seconds, effect) = match level {
        AlertLevel::Advisory => (50, 3, WeatherEffect::Advisory),
        AlertLevel::Watch => (28, 2, WeatherEffect::Watch),
        AlertLevel::Warning => (0, 1, WeatherEffect::Warning),
        AlertLevel::None => (0, 1, WeatherEffect::None),
    };
    Pulse { shade: Shade { hue_deg, saturation: 254 }, fade_seconds, strength: 1.0, effect }
}

// Blues for water, paler and slower for snow; violet for freezing rain,
// purple and quick for thunder. Heavier falls pulse brighter.
const fn precip_pulse(precip: Precip, intensity: Intensity) -> Pulse {
    let strength = match intensity {
        Intensity::Heavy => 1.0,
        Intensity::Moderate => 0.8,
        Intensity::Light => 0.6,
    };
    let (hue_deg, saturation, fade_seconds, effect) = match precip {
        Precip::Drizzle => (205, 170, 3, WeatherEffect::Drizzle),
        Precip::FreezingRain => (280, 230, 2, WeatherEffect::FreezingRain),
        Precip::Snow => (195, 150, 3, WeatherEffect::Snow),
        Precip::Thunderstorm => (270, 254, 1, WeatherEffect::Thunderstorm),
        Precip::Rain => (228, 254, 2, WeatherEffect::Rain),
        Precip::None => (228, 254, 2, WeatherEffect::None),
    };
    Pulse { shade: Shade { hue_deg, saturation }, fade_seconds, strength, effect }
}

const fn zigbee_hue(degrees: u16) -> u8 {
    ((degrees % 360) as u32 * 254 / 360) as u8
}

/// What the sky shows on `role`, if anything; pure. Only on the weather
/// lights (`lights`: the zones' declared ones, WEATHER_LIGHTS). `lit`: the
/// program has the light on; `mode`: its zone's. On the alert lights, a
/// warning shows in daylight and off modes whether lit or not (it's an
/// alarm); an advisory or watch only on a lit light in daylight mode.
/// Failing that, on the precipitation lights, rain or snow shows in
/// daylight mode: dimmer if the light is otherwise off.
pub fn weather_pulse(
    role: Role,
    lit: bool,
    mode: Mode,
    config: &ProgramConfig,
    sky: &Sky,
    lights: &WeatherLights,
) -> Option<Pulse> {
    let alert = sky.alert();
    if !config.weather_enabled {
        return None;
    }
    let daylight = mode == Mode::Daylight;
    if alert != AlertLevel::None && lights.alerts.contains(role) {
        let shown = if alert == AlertLevel::Warning { daylight || mode == Mode::Off } else { daylight && lit };
        if shown {
            return Some(alert_pulse(alert));
        }
    }
    match sky.conditions {
        Some(now) if daylight && now.precip != Precip::None && lights.precip.contains(role) => {
            let mut pulse = precip_pulse(now.precip, now.intensity);
            if !lit {
                pulse.strength *= UNLIT_STRENGTH;
            }
            Some(pulse)
        }
        _ => None,
    }
}

#[derive(Clone, Copy, Default, Debug)]
struct Light {
    base: Option<LightTarget>, // the program's latest
    pulse: Option<Pulse>,      // shown instead, while the weather has the light
    beat: Option<i64>,         // the pulse's beat last shown
}

/// The stage between the running program and LightOutput: passes each
/// LightTarget through as the OutputTarget, except on lights the weather has
/// taken over (weather_pulse), which it pulses itself: a fade each beat of
/// the Ticks' time (fade_seconds), whenever the Ticks come. When the weather
/// lets a light go, the program's latest target for it goes out again, with
/// a quick transition. Re-checked when the sky changes (SkyChanged: at once)
/// and every Tick (the mode, the program's lights, the pulse's beat).
pub struct Overlays {
    post: Post,
    installation: Arc<Installation>,
    sky: Sky,
    modes: PerZone<Mode>, // each zone's; Daylight to start
    config: ProgramConfig,
    peaks: Peaks,
    min_levels: MinLevels,
    lights: PerRole<Light>,
    now: Instant, // the latest Tick's
}

impl Overlays {
    pub fn new(post: Post, installation: Arc<Installation>) -> Self {
        Overlays {
            post,
            installation,
            sky: Sky::default(),
            modes: PerZone::default(),
            config: ProgramConfig::default(),
            peaks: DEFAULT_PEAKS,
            min_levels: DEFAULT_MIN_LEVELS,
            lights: PerRole::default(),
            now: Instant::ZERO,
        }
    }

    fn light_target(&mut self, e: &LightTarget) {
        if !is_light(e.role) {
            return;
        }
        let light = &mut self.lights[e.role];
        light.base = Some(*e);
        if light.pulse.is_none() {
            self.post.send(OutputTarget::from(e.0));
        }
    }

    // Each light to what the sky shows on it now: a pulse started or changed,
    // one fading on this beat, or one let go.
    fn reconcile(&mut self) {
        for role in lights() {
            let light = self.lights[role];
            let lit = light.base.is_some_and(|b| b.state.on);
            let topology = &self.installation.topology;
            let Some(zone) = topology.zone_of[role] else { continue };
            let Some(pulse) = weather_pulse(role, lit, self.modes[zone], &self.config, &self.sky, &topology.weather)
            else {
                if light.pulse.is_some() {
                    self.release(role);
                }
                continue;
            };
            // Every pulse beats by the same clock, so lights with the same
            // tempo breathe together.
            let beat = self.now.since_boot() / Duration::secs(pulse.fade_seconds as i64);
            let changed = light.pulse != Some(pulse);
            self.lights[role].pulse = Some(pulse);
            if changed || light.beat != Some(beat) {
                self.lights[role].beat = Some(beat);
                self.show(role, &pulse, beat % 2 == 0);
            }
        }
    }

    fn show(&self, role: Role, pulse: &Pulse, bright: bool) {
        let min = self.min_levels[role];
        let peak = MAX_LEVEL as f64 * self.peaks[role] as f64 / 100.0 * pulse.strength;
        let top = min.max(peak.round().max(1.0) as u8);
        let bottom = ((top as f64 * DIM_FRACTION).round() as u8).clamp(min, top);

        self.post.send(OutputTarget {
            change: LightChange {
                role,
                state: LightState {
                    on: true,
                    level: if bright { top } else { bottom },
                    color: HueSat { hue: zigbee_hue(pulse.shade.hue_deg), saturation: pulse.shade.saturation }.into(),
                },
                level_transition: Transition::tenths(pulse.fade_seconds as u16 * 10 - 1), // nearly the whole fade
                color_transition: COLOR_TRANSITION,
            },
            weather_effect: pulse.effect,
            pulse_phase: if bright { PulsePhase::Bright } else { PulsePhase::Dim },
        });
    }

    fn release(&mut self, role: Role) {
        let light = &mut self.lights[role];
        light.pulse = None;
        light.beat = None;
        // Back to the program's latest (off, if it hasn't sent one), quickly.
        let mut back = light.base.map_or(LightChange { role, ..LightChange::default() }, |b| b.0);
        back.level_transition = RELEASE_TRANSITION;
        back.color_transition = RELEASE_TRANSITION;
        self.post.send(OutputTarget::from(back));
    }
}

impl Module for Overlays {
    fn on(&mut self, e: &Event) {
        match e {
            Event::LightTarget(e) => self.light_target(e),
            Event::Tick(e) => {
                self.now = e.at;
                self.reconcile();
            }
            Event::ModeChanged(e) => self.modes[e.zone] = e.mode,
            Event::ConfigChanged(e) => {
                self.config = e.config;
                self.peaks = e.peaks;
                self.min_levels = e.min_levels;
            }
            Event::SkyChanged(e) => {
                self.sky = e.sky;
                self.reconcile();
            }
            _ => {}
        }
    }
}
