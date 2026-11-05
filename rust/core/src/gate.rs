//! How open a gate is.

use ev::{ProgramConfig, Rule, RuleSource, Says, TimeReading, Window};

// One rule's answer, before Says: how open, or None for no answer.
type Answer = Option<f64>;

fn window(window: &Window, time: &TimeReading, config: &ProgramConfig) -> Answer {
    let hours = window.hours.value(config);
    let fade = window.fade.value(config);
    let now = time.since_midnight();
    if !window.days.value(config).contains(time.weekday()) || !hours.contains(now) {
        return Some(0.0);
    }
    if fade <= ev::Duration::ZERO {
        return Some(1.0);
    }
    // How far into the fade at either edge, 0 to 1 and beyond.
    let fade_in = (now - hours.start).as_secs_f64() / fade.as_secs_f64();
    let fade_out = (hours.end - now).as_secs_f64() / fade.as_secs_f64();
    Some(fade_in.min(fade_out).min(1.0))
}

// What `rule` says: its answer, unless it's the case it leaves to the rules after it.
fn says(rule: &Rule, time: &TimeReading, config: &ProgramConfig) -> Answer {
    let a = match &rule.source {
        RuleSource::Always => Some(1.0),
        RuleSource::Window(w) => window(w, time, config),
    };
    match rule.says {
        Says::OnOnly => a.filter(|&a| a > 0.0),
        Says::OffOnly => a.filter(|&a| a <= 0.0),
        Says::Both => a,
    }
}

/// How open `gate` is at `time`: 0 (the lights off) to 1, in between while
/// fading. The first rule with an answer decides; when none answers, 0.
pub fn openness(gate: &[Rule], time: &TimeReading, config: &ProgramConfig) -> f64 {
    gate.iter().find_map(|rule| says(rule, time, config)).unwrap_or(0.0)
}
