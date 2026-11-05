//! The button and the modes: red alert, all off, click timing, tuning.

use ev::*;
use sunsim_tests::*;

fn press(press: Press, at: Instant) -> ButtonPressed {
    ButtonPressed { press, at, ..ButtonPressed::default() }
}

#[test]
fn double_press_red_alert_pulses_single_press_daylight_resumes() {
    let mut h = Harness::new();
    h.tick(1);
    h.pair_bulb(WALL_A, 0xA1, 0x0001);
    h.pair_bulb(WORK, 0xB2, 0x0002);
    h.clear_log();

    h.post(press(Press::Double, h.now));
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::RedAlert);
    let red = h.all::<SetLight>();
    assert_eq!(red.len(), 2); // one immediate pulse per bulb
    assert_eq!(hue_sat(red[0].color), Some(HueSat { hue: 0, saturation: 254 }));

    h.clear_log();
    h.tick(3);
    let pulses = h.all::<SetLight>();
    assert_eq!(pulses.len(), 6);
    assert!(pulses[0].level.is_some() && pulses[2].level.is_some() && pulses[0].level != pulses[2].level); // bright/dim alternate
    assert!(pulses[0].color.is_none()); // already red: only the level moves

    h.clear_log();
    h.post(press(Press::Single, h.now));
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::Daylight);
    let back = h.all::<SetLight>();
    assert!(back.len() == 2 && back[0].level.is_some()); // daylight at once
    assert!(mireds(back[0].color).is_some() || xy(back[0].color).is_some());
}

#[test]
fn hold_all_off_the_daylight_program_is_paused_until_resumed() {
    let mut h = Harness::new();
    h.tick(1);
    h.pair_bulb(WALL_B, 0xC3, 0x0003);
    h.clear_log();
    h.post(ButtonPressed { source: Source::Console, ..press(Press::Hold, h.now) });
    let off = h.all::<SetLight>();
    assert!(off.len() == 1 && off[0].power == Some(false));

    h.clear_log();
    h.post(ProgramTick { time: noon(), transition: Transition::secs(60) });
    assert!(h.all::<LightTarget>().is_empty());
    assert!(h.all::<SetLight>().is_empty());
}

#[test]
fn a_bulb_paired_during_red_alert_joins_in_red() {
    let mut h = Harness::new();
    h.tick(1);
    h.post(ButtonPressed { source: Source::Console, ..press(Press::Double, h.now) });
    h.clear_log();
    h.pair_bulb(WALL_C, 0xC3, 0x0003);
    let lights = h.all::<SetLight>();
    assert!(lights.len() == 1 && lights[0].power == Some(true));
    assert_eq!(hue_sat(lights[0].color).map(|c| c.hue), Some(0));
}

#[test]
fn ambiguous_on_toggle_two_within_450_ms_double_one_alone_single() {
    let mut h = Harness::new();
    h.tick(1);
    h.post(press(Press::Ambiguous, h.now));
    h.post(press(Press::Ambiguous, h.now + ms(200)));
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::RedAlert);

    let mut g = Harness::new();
    g.tick(1);
    g.post(press(Press::Ambiguous, g.now));
    assert!(g.all::<ModeChanged>().is_empty()); // still waiting for a possible second click
    g.tick(1);
    let modes = g.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::Daylight);
}

#[test]
fn an_ambiguous_press_alone_resolves_as_the_window_ends_a_wake_not_at_the_next_tick() {
    let mut h = Harness::new();
    h.tick(1);
    let pressed = h.now + ms(100);
    h.post(press(Press::Ambiguous, pressed));
    let wakes = h.all::<WakeAt>();
    assert_eq!(wakes.last().map(|w| w.at), Some(pressed + ms(450))); // asked to be woken as the window ends
    assert!(h.all::<ModeChanged>().is_empty());
    h.post(Wake { at: pressed + ms(450) });
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::Daylight);
}

#[test]
fn red_alert_keeps_its_beat_a_second_tick_in_the_same_second_doesnt_flip_it() {
    let mut h = Harness::new();
    h.tick(1);
    h.pair_bulb(WALL_A, 0xA1, 0x0001);
    h.post(press(Press::Double, h.now));
    h.tick(1);
    h.clear_log();
    h.post(Tick { at: h.now + ms(300), time: noon() }); // late, or a duplicate: same second
    assert!(h.all::<SetLight>().is_empty());
    h.tick(1);
    assert_eq!(h.all::<SetLight>().len(), 1); // the next second flips it
}

#[test]
fn ambiguous_then_a_definite_press_within_the_window_the_definite_press_wins() {
    let mut h = Harness::new();
    h.tick(1);
    h.post(press(Press::Ambiguous, h.now));
    h.post(press(Press::Hold, h.now + ms(100)));
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::Off);
    h.tick(1); // nothing left pending
    assert_eq!(h.all::<ModeChanged>().len(), 1);
}

#[test]
fn ambiguous_a_second_click_after_the_window_is_a_new_first_click() {
    let mut h = Harness::new();
    h.tick(1);
    h.post(press(Press::Ambiguous, h.now));
    h.post(press(Press::Ambiguous, h.now + ms(600))); // too late for a double
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::Daylight); // the first was a single
    h.post(press(Press::Ambiguous, h.now + ms(800))); // pairs with the second
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 2 && modes[1].mode == Mode::RedAlert);
}

#[test]
fn tuning_bulbs_at_their_minimums_tune_sets_levels_save_makes_them_the_minimums() {
    let mut h = Harness::new();
    h.tick(1);
    let mut mins = DEFAULT_MIN_LEVELS;
    mins[WORK] = 12;
    h.post(ConfigChanged { min_levels: mins, ..ConfigChanged::default() });
    h.clear_log();

    h.post(ModeRequested { mode: Mode::Tuning, zone: None });
    let start = h.all::<LightTarget>();
    assert_eq!(start.len(), LIGHT_COUNT);
    for t in &start {
        assert!(t.state.on && xy(Some(t.state.color)).is_some()); // the sunset's amber
        assert_eq!(t.state.level, if t.role == WORK { 12 } else { 1 });
    }

    h.clear_log();
    h.post(TuneLights { lights: Group::Wall.into(), level: 9, kelvin: None });
    for t in h.all::<LightTarget>() {
        assert_eq!(t.state.level, if t.role.group == Group::Wall { 9 } else { 12 });
    }

    h.clear_log();
    h.post(TuneSave);
    let saved = h.all::<SetMinLevel>();
    assert_eq!(saved.len(), members(Group::Wall).len());
    assert!(saved.iter().all(|s| s.level == 9 && matches!(s.lights, Selection::Role(r) if r.group == Group::Wall)));

    h.clear_log();
    h.post(press(Press::Single, h.now));
    let modes = h.all::<ModeChanged>();
    assert!(modes.len() == 1 && modes[0].mode == Mode::Daylight); // daylight, sky and all
}
