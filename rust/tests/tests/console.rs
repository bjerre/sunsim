//! The console's commands, on the real core: what each posts, and what it
//! says back.

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use ev::*;
use platform::console::{clock_text, duration_text, split_words};
use platform::{Console, ConsoleBoard, StatusSlot, WifiStore};
use sunsim_tests::*;

struct Board {
    now: Rc<Cell<Instant>>,
    restarted: bool,
}

impl ConsoleBoard for Board {
    fn now(&self) -> Instant {
        self.now.get()
    }
    fn restart(&mut self) {
        self.restarted = true;
    }
}

// The core (the harness) with a status slot and a console, both on its bus.
struct Rig {
    h: Harness,
    console: Console<Nvs, Board>,
    now: Rc<Cell<Instant>>,
}

impl Rig {
    fn new() -> Self {
        let mut h = Harness::new();
        let slot = Rc::new(RefCell::new(StatusSlot::default()));
        h.bus.attach(&slot);
        let shared = slot.borrow().clone();
        let now = Rc::new(Cell::new(Instant::ZERO));
        let wifi = WifiStore::new(Nvs::default(), None);
        let console =
            Console::new(h.bus.post().clone(), shared, wifi, Board { now: now.clone(), restarted: false }, 120);
        Rig { h, console, now }
    }

    // A second's Tick, so the core publishes a status.
    fn running(mut self) -> Self {
        self.h.tick(1);
        self.now.set(self.h.now);
        self.h.clear_log();
        self
    }

    // Types `line`; the reply, once what it posted has been handled.
    fn ty(&mut self, line: &str) -> String {
        let reply = self.console.line(line);
        self.h.bus.drain();
        reply
    }
}

fn failed(reply: &str) -> bool {
    reply.contains("Command returned non-zero error code: 0x1 (ERROR)")
}

#[test]
fn lines_split_at_spaces_with_quotes_and_escapes() {
    assert_eq!(split_words("  wifi add \"My Net\" se\\\"cret  "), ["wifi", "add", "My Net", "se\"cret"]);
    assert_eq!(split_words("wifi add \"\""), ["wifi", "add", ""]); // an empty password, quoted
    assert!(split_words("   ").is_empty());
}

#[test]
fn unknown_and_failing_commands_say_so_in_esp_consoles_words() {
    let mut r = Rig::new().running();
    assert_eq!(r.ty("frobnicate"), "Unrecognized command\n");
    assert!(failed(&r.ty("pair")));
    assert_eq!(r.ty(""), "");
    let help = r.ty("help");
    assert!(help.contains("status \n  Time, day length") && help.contains("factory_reset"));
}

#[test]
fn before_the_first_status_commands_that_need_it_say_wait() {
    let mut r = Rig::new();
    let reply = r.ty("status");
    assert!(reply.starts_with("still starting up") && failed(&reply));
}

#[test]
fn status_shows_the_time_the_day_the_mode_and_each_device() {
    let mut r = Rig::new().running();
    r.h.pair_bulb(WALL_A, 0xA1, 0x0001);
    r.h.tick(1);
    let reply = r.ty("status");
    assert!(reply.contains("time    Tue 12:00\n"), "{reply}");
    assert!(reply.contains("day     ") && reply.contains("(sunrise "));
    assert!(reply.contains("sky     no forecast yet (clear assumed)"));
    assert!(reply.contains("mode    office: daylight - lights may be on (gate open)\n"), "{reply}");
    assert!(reply.contains("mode    desk-downlight: daylight - lights may be on (gate open)\n"));
    assert!(reply.contains("mode    bookcase-grow-light: daylight - lights off (gate shut)\n"));
    assert!(reply.contains("  WallA  *  online, heard 1s ago\n"), "{reply}");
    assert!(reply.contains("  Work   -  not paired\n"));
}

#[test]
fn roles_pair_unpair_and_open_post_the_requests() {
    let mut r = Rig::new().running();
    assert_eq!(r.ty("pair wallc"), "");
    assert_eq!(r.h.all::<PairRequested>()[0].role, WALL_C);
    r.h.tick(1); // the status shows it from the next second
    assert!(r.ty("roles").contains("Armed: next joining device becomes WallC"));
    r.ty("pair cancel");
    assert_eq!(r.h.all::<PairCanceled>().len(), 1);

    let reply = r.ty("pair lamp");
    assert!(
        reply.starts_with("unknown role 'lamp'\nroles: WallA WallB WallC WallD WallE Work Desk Grow Button\n")
            && failed(&reply)
    );

    r.ty("unpair wall");
    assert_eq!(r.h.all::<UnpairRequested>()[0].roles, Selection::Group(Group::Wall));
    r.ty("open 60");
    assert_eq!(r.h.all::<OpenNetwork>().last().unwrap().secs, 60);
    assert!(failed(&r.ty("open 255")));
}

#[test]
fn config_shows_and_changes_one_setting_at_a_time() {
    let mut r = Rig::new().running();
    let all = r.ty("config");
    assert!(all.contains("  hours       08:00-19:00    when the lights may be on\n"), "{all}");
    assert_eq!(all.lines().count(), SETTINGS.len());

    let reply = r.ty("config hours 07:00-20:00");
    assert!(reply.contains("hours       07:00-20:00"));
    assert_eq!(r.h.all::<SetConfig>()[0].config.active, DaySpan::new(hours(7), hours(20)));

    let reply = r.ty("config hours 20:00-07:00");
    assert!(reply.starts_with("hours takes HH:MM-HH:MM") && failed(&reply));
    let usage = r.ty("config nonsense 1");
    assert!(usage.starts_with("usage: config [<setting> <value>]\n  hours       HH:MM-HH:MM\n") && failed(&usage));
}

#[test]
fn intensity_and_minlevel_go_to_lights_only() {
    let mut r = Rig::new().running();
    let reply = r.ty("intensity wall 60");
    assert!(reply.contains("  WallA   60%\n") && reply.contains("  Work    90%\n"), "{reply}");
    assert_eq!(r.h.all::<SetIntensity>()[0].percent, 60);
    assert!(r.ty("intensity Button 50").starts_with("'Button' isn't a light"));
    assert!(failed(&r.ty("intensity wall 0")));

    r.ty("minlevel Work 12");
    assert_eq!(r.h.all::<SetMinLevel>()[0].level, 12);
    assert!(failed(&r.ty("minlevel Work 255")));
}

#[test]
fn tune_switches_to_tuning_sets_levels_and_saves_them() {
    let mut r = Rig::new().running();
    assert!(failed(&r.ty("tune save"))); // not tuning yet

    assert!(r.ty("tune").starts_with("tuning: every bulb steady"));
    assert_eq!(r.h.all::<ModeChanged>().last().unwrap().mode, Mode::Tuning);
    r.h.tick(1); // the status catches up

    r.ty("tune wall 9 2000");
    let tuned = r.h.all::<TuneLights>();
    assert!(tuned.len() == 1 && tuned[0].level == 9 && tuned[0].kelvin == Some(2000));
    assert!(failed(&r.ty("tune wall 9 99999")));
    assert!(failed(&r.ty("tune wall 300")));

    assert!(r.ty("tune save").starts_with("saved as the minimum levels"));
    assert_eq!(r.h.all::<SetMinLevel>().len(), members(Group::Wall).len());
    r.ty("tune off");
    assert_eq!(r.h.all::<ModeChanged>().last().unwrap().mode, Mode::Daylight);
}

#[test]
fn press_goes_through_the_whole_button_path() {
    let mut r = Rig::new().running();
    r.ty("press double");
    let press = r.h.all::<ButtonPressed>()[0];
    assert!(press.source == Source::Console && press.press == Press::Double && press.at == r.now.get());
    assert_eq!(r.h.all::<ModeChanged>()[0].mode, Mode::RedAlert);
    assert!(failed(&r.ty("press triple")));
}

#[test]
fn fastday_sync_restart_and_factory_reset() {
    let mut r = Rig::new().running();
    assert_eq!(r.ty("fastday"), "fast-day off\n");
    r.ty("fastday on");
    assert_eq!(r.h.all::<SetFastDay>()[0], SetFastDay { on: true, day_length: secs(120) });
    r.ty("fastday on 30");
    assert_eq!(r.h.all::<SetFastDay>()[1].day_length, secs(30));
    assert!(failed(&r.ty("fastday on 5")));
    r.ty("fastday off");
    assert!(!r.h.all::<SetFastDay>()[2].on);

    assert!(r.ty("sync").starts_with("syncing in the background"));
    assert_eq!(r.h.all::<SyncRequested>().len(), 1);
    r.ty("restart");
    assert!(r.console.board().restarted);
    r.ty("factory_reset");
    assert_eq!(r.h.all::<FactoryReset>().len(), 1);
}

#[test]
fn weather_shows_the_sky_and_simulates_it() {
    let mut r = Rig::new().running();
    let reply = r.ty("weather");
    assert!(reply.starts_with("now      no live conditions (none fetched in the last 45 min)\nalerts   none\n"));
    assert!(reply.contains("precip   WallC\nalerts   WallB,WallD\n"), "{reply}");

    r.ty("weather simulate snow heavy");
    let now = r.h.all::<ConditionsUpdated>()[0].now;
    assert!(now.precip == Precip::Snow && now.intensity == Intensity::Heavy && now.cloud_percent >= 90);
    r.ty("weather simulate warning");
    let alerts = r.h.all::<AlertsUpdated>()[0].alerts;
    assert!(alerts.count == 1 && alerts.items[0].level == AlertLevel::Warning);
    assert_eq!(alerts.items[0].event(), "Simulated warning");
    r.h.tick(1);
    let reply = r.ty("weather");
    assert!(
        reply.contains("now      90% cloud, heavy snow")
            && reply.contains("alert    Simulated warning (warning), 59 min left"),
        "{reply}"
    );

    r.ty("weather simulate clear");
    assert_eq!(r.h.all::<AlertsUpdated>()[1].alerts.count, 0);
    assert!(failed(&r.ty("weather simulate hail")));
}

#[test]
fn wifi_lists_adds_and_removes_networks() {
    let mut r = Rig::new().running();
    assert!(r.ty("wifi").starts_with("no Wi-Fi networks"));
    assert!(r.ty("wifi add \"My Net\" secret").starts_with("added \"My Net\""));
    r.ty("wifi add cafe");
    assert_eq!(
        r.ty("wifi"),
        "  1. \"My Net\" (password set)\n  2. \"cafe\" (open network)\nthe strongest one in range is used at each clock sync\n"
    );
    assert!(r.ty("wifi add cafe pw").starts_with("updated the password"));
    assert!(failed(&r.ty("wifi remove nowhere")));
    r.ty("wifi remove cafe");
    r.ty("wifi clear");
    assert!(r.ty("wifi").starts_with("no Wi-Fi networks"));
    assert!(failed(&r.ty("wifi frob")));
}

#[test]
fn times_and_durations_as_the_status_shows_them() {
    assert_eq!(clock_text(6.5), "06:30");
    assert_eq!(clock_text(24.0), "24:00"); // a window ending at midnight
    assert_eq!(clock_text(24.5), "00:30"); // past it: the next day
    assert_eq!(clock_text(5.0 + 59.6 / 60.0), "06:00"); // to the nearest minute
    assert_eq!(duration_text(13.25), "13h 15m");
}
