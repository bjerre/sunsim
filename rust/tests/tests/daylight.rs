//! The daylight program: the sun curve along the wall, the clock guard, clouds, minimum levels.

use ev::*;
use sunsim_core::{cloud_cover, openness};
use sunsim_tests::*;

fn program_tick(time: TimeReading) -> ProgramTick {
    ProgramTick { time, transition: Transition::secs(60) }
}

// Each wall light's target, east to west.
fn wall_targets(h: &mut Harness, time: TimeReading) -> Vec<LightState> {
    h.clear_log();
    h.post(program_tick(time));
    h.all::<LightTarget>().into_iter().filter(|t| t.role.group == Group::Wall).map(|t| t.state).collect()
}

fn pair_wall(h: &mut Harness) {
    for &wall in members(Group::Wall) {
        h.pair_bulb(wall, 0xA0 + wall.index as u64, 0x0010 + wall.index as u16);
    }
}

#[test]
fn clock_not_set_lights_held_off() {
    let mut h = Harness::new();
    let unset = TimeReading { valid: false, ..noon() };
    h.tick_at(1, unset);
    h.pair_bulb(WALL_A, 0xA1, 0x0001);
    h.clear_log();
    h.post(program_tick(unset));
    let targets = h.all::<LightTarget>();
    assert_eq!(targets.len(), LIGHT_COUNT);
    assert!(targets.iter().all(|t| !t.state.on));
}

#[test]
fn the_gate_the_active_hours_fade_at_each_edge_the_first_rule_with_an_answer_decides() {
    let c = ProgramConfig::default(); // 08:00-19:00, Monday to Friday, 10 min fades
    let office = ZONES[0].gate; // the active hours
    assert_eq!(openness(office, &noon(), &c), 1.0);
    assert_eq!(openness(office, &at(hours(7) + mins(59)), &c), 0.0);
    assert!((openness(office, &at(hours(8) + mins(5)), &c) - 0.5).abs() < 1e-9);
    assert!((openness(office, &at(hours(18) + mins(55)), &c) - 0.5).abs() < 1e-9);
    let mut saturday = noon();
    saturday.local += Duration::days(4);
    assert_eq!(openness(office, &saturday, &c), 0.0);

    let nine_to_five = Window::hours(Tunable::Fixed(DaySpan::new(hours(9), hours(17))));
    let noon_to_one = Window::hours(Tunable::Fixed(DaySpan::new(hours(12), hours(13))));
    assert_eq!(openness(&[], &noon(), &c), 0.0); // no rule answers: off
    assert_eq!(openness(&[Rule::ALWAYS], &at(hours(3)), &c), 1.0);
    // Only its "off": inside the hours the next rule decides.
    let closes = [nine_to_five.closes_only(), noon_to_one.rule()];
    assert_eq!(openness(&closes, &at(hours(10)), &c), 0.0);
    assert_eq!(openness(&closes, &at(hours(12) + mins(30)), &c), 1.0);
    assert_eq!(openness(&closes, &at(hours(20)), &c), 0.0);
    // Only its "on": outside the hours the next rule decides.
    let opens = [nine_to_five.opens_only(), Window::hours(Tunable::Fixed(DaySpan::new(hours(18), hours(20)))).rule()];
    assert_eq!(openness(&opens, &at(hours(10)), &c), 1.0);
    assert_eq!(openness(&opens, &at(hours(19)), &c), 1.0);
    assert_eq!(openness(&opens, &at(hours(21)), &c), 0.0);

    // A setting, not a fixed value: it follows the config.
    let work = [Window::hours(ev::setting!(work)).rule()];
    let late = ProgramConfig { work: DaySpan::new(hours(13), hours(22)), ..c };
    assert!(openness(&work, &at(hours(10)), &c) == 1.0 && openness(&work, &at(hours(10)), &late) == 0.0);
}

#[test]
fn each_light_shows_its_declared_look_faded_by_the_gate() {
    assert!(members(Group::Wall).iter().all(|&r| matches!(TOPOLOGY.look_of[r], Look::SunCurve)));
    assert!(matches!(TOPOLOGY.look_of[WORK], Look::WorkCurve(_)));

    let mut h = Harness::new();
    h.tick(1);
    let mut work = |t| {
        h.clear_log();
        h.post(program_tick(t));
        h.all::<LightTarget>()
            .into_iter()
            .find(|t| t.role == WORK)
            .map_or(LightState { level: 99, ..LightState::default() }, |t| t.state)
    };
    let before = work(at(hours(7) + mins(30)));
    let fading = work(at(hours(8) + mins(5)));
    let full = work(at(hours(8) + mins(30)));
    assert!(!before.on);
    assert!(fading.on && full.on && fading.level < full.level && fading.level as i32 * 2 > full.level as i32 - 4);
    assert_eq!(full.color.mireds(), Some(Mireds(ProgramConfig::default().work_cool_mireds)));
}

#[test]
fn intensity_scales_the_target_level_program_tick_ramps_over_the_interval() {
    let mut h = Harness::new();
    h.tick(1);
    let mut peaks = DEFAULT_PEAKS;
    peaks[WALL_A] = 50;
    h.post(ConfigChanged { peaks, ..ConfigChanged::default() });
    h.clear_log();
    h.post(program_tick(noon()));
    let targets = h.all::<LightTarget>();
    assert_eq!(targets.len(), LIGHT_COUNT);
    assert!(targets[0].state.on && targets[1].state.on);
    assert!(targets[0].state.level as i32 * 2 <= targets[1].state.level as i32 + 2); // WallA ~half of WallB
    assert!(targets[0].level_transition == Transition::secs(60) && targets[0].color_transition == Transition::secs(60));
}

#[test]
fn the_wall_dims_away_from_the_sun_in_order_along_the_wall() {
    let mut h = Harness::new();
    h.tick(1);
    pair_wall(&mut h);
    // Late afternoon at 0°E: the sun is in the west.
    let levels: Vec<u8> = wall_targets(&mut h, noon()).iter().map(|s| s.level).collect();
    assert_eq!(levels.len(), 5);
    assert!(levels.is_sorted() && levels[0] < levels[4]); // E dimmest, W brightest
}

#[test]
fn at_sunset_the_amber_glow_sweeps_the_wall_from_the_west_end() {
    let mut h = Harness::new();
    let dusk = at(hours(12) + mins(55)); // 17:55 UTC at 0°E: the sun ~2° up, in the west-northwest
    h.tick_at(1, dusk);
    pair_wall(&mut h);
    // How amber each wall light is: along the blackbody line, x grows as it warms.
    let wall_amber = |h: &mut Harness| -> Vec<f32> {
        wall_targets(h, dusk).iter().map(|s| s.color.xy().expect("amber is xy").x).collect()
    };

    let swept = wall_amber(&mut h);
    assert_eq!(swept.len(), 5);
    assert!(swept.is_sorted() && swept[0] < swept[4]); // the west end warmest

    h.post(ConfigChanged {
        config: ProgramConfig { amber_sweep_deg: 0, ..ProgramConfig::default() },
        ..ConfigChanged::default()
    });
    let even = wall_amber(&mut h);
    assert!(even.iter().all(|&x| x == even[0]));
    assert!(swept.iter().all(|&x| x <= even[0])); // the sweep only holds amber back
    assert!(even[0] - swept[4] < 0.01); // and barely at the end nearest the sun
}

#[test]
fn cloud_cover_interpolated_by_local_hour_fast_day_reads_todays() {
    let half = cloud_cover(&tuesday_forecast(), &at(hours(12) + mins(30))).unwrap();
    assert!((half - 0.40).abs() < 1e-9);

    let mut tomorrow = noon();
    tomorrow.local += Duration::days(1);
    let next = cloud_cover(&tuesday_forecast(), &tomorrow).unwrap();
    assert!((next - 0.90).abs() < 1e-9); // hour 36, with no data at 37

    let mut sim = TimeReading { local: LocalDays::ymd(2024, 1, 1) + hours(12), simulated: true, ..noon() }; // fast-day's week
    let today = cloud_cover(&tuesday_forecast(), &sim).unwrap();
    assert!((today - 0.20).abs() < 1e-9);

    sim.simulated = false;
    assert!(cloud_cover(&tuesday_forecast(), &sim).is_none()); // a date the forecast doesn't cover
    assert!(cloud_cover(&CloudCover::default(), &noon()).is_none()); // no forecast yet
    assert!(cloud_cover(&tuesday_forecast(), &at(hours(3))).is_none()); // no data for 03:00
}

#[test]
fn clouds_dim_the_wall_cool_it_and_flatten_its_sun_direction() {
    let mut clear = Harness::new();
    clear.tick(1);
    let sunny = wall_targets(&mut clear, noon());

    let mut gray = Harness::new();
    gray.tick(1);
    gray.post(CloudCoverUpdated { forecast: CloudCover { day: Some(TUESDAY), percent: [100; CloudCover::HOURS] } });
    let cloudy = wall_targets(&mut gray, noon());

    assert!(sunny.len() == 5 && cloudy.len() == 5);
    assert!(sunny[0].level < sunny[4].level); // clear: brighter towards the sun
    assert_eq!(cloudy[0].level, cloudy[4].level); // overcast: diffuse, even
    assert!(cloudy[4].level < sunny[4].level); // and dimmer
    assert!(cloudy[4].color.mireds().unwrap().0 < sunny[4].color.mireds().unwrap().0); // and cooler

    gray.post(ConfigChanged {
        config: ProgramConfig { clouds_enabled: false, ..ProgramConfig::default() },
        ..ConfigChanged::default()
    });
    let ignored = wall_targets(&mut gray, noon());
    assert_eq!(ignored[4].level, sunny[4].level);
}

#[test]
fn minimum_levels_keep_the_wall_lit_down_to_sunset_then_off() {
    let dusk = at(hours(12) + mins(55)); // the sun ~2 deg up
    let west_level = |h: &mut Harness, t| {
        h.clear_log();
        h.post(program_tick(t));
        h.all::<LightTarget>()
            .into_iter()
            .find(|t| t.role == WALL_E)
            .map_or(-1, |t| if t.state.on { t.state.level as i32 } else { 0 })
    };
    let mut h = Harness::new();
    h.tick(1);
    let dim = west_level(&mut h, dusk);

    let mut mins_ = DEFAULT_MIN_LEVELS;
    mins_[WALL_E] = 40;
    h.post(ConfigChanged { min_levels: mins_, ..ConfigChanged::default() });
    let floored = west_level(&mut h, dusk);
    assert!((1..40).contains(&dim));
    assert!(floored >= 40);

    let night = at(hours(13) + mins(30)); // after sunset
    assert_eq!(west_level(&mut h, night), 0);
}
