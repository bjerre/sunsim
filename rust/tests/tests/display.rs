//! The status display's picture, from real core status: what it says, and
//! the pixels it draws (a sky, the sun where it is, a cloud when there is
//! one, the text over it, a dot per device).

use std::cell::RefCell;
use std::rc::Rc;

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use ev::*;
use platform::StatusSlot;
use platform::display::{
    BLACK, Frame, HEIGHT, ONLINE, SILENT, SKIES, Screen, UNPAIRED, WIDTH, dot_at, draw_splash, link_color, rgb, sky_of,
    sun_at,
};
use platform::presentation::Tone;
use sunsim_tests::*;

// The core's latest status, after `setup`, at noon.
fn status(setup: impl FnOnce(&mut Harness)) -> std::sync::Arc<StatusSnapshot> {
    status_at(noon(), setup)
}

// The same, at `time`: a second's ticks before `setup` and two after (a
// status shows what changed in a second from the next).
fn status_at(time: TimeReading, setup: impl FnOnce(&mut Harness)) -> std::sync::Arc<StatusSnapshot> {
    let mut h = Harness::new();
    let slot = Rc::new(RefCell::new(StatusSlot::default()));
    h.bus.attach(&slot);
    // Verona, Wisconsin, where the clock's five hours behind UTC is local time.
    let config = ProgramConfig { latitude_deg: 42.99, longitude_deg: -89.53, ..ProgramConfig::default() };
    h.post(ConfigChanged { config, ..ConfigChanged::default() });
    h.tick_at(1, time);
    setup(&mut h);
    h.tick_at(2, time);
    slot.borrow().get().expect("a status")
}

fn drawn(screen: &Screen) -> Frame {
    let mut frame = Frame::default();
    screen.draw(&mut frame);
    frame
}

// The center of a device's dot.
fn dot_color(frame: &Frame, role: Role) -> Rgb565 {
    frame.at(dot_at(role) + Point::new(5, 5))
}

#[test]
fn the_time_the_day_and_the_next_sunrise_or_sunset() {
    let st = status(|_| {});
    let s = Screen::of(&st).view;
    assert_eq!(s.time, "12:00");
    assert_eq!(s.weekday, "Tue");
    assert!(s.day_length.starts_with("Day ") && s.day_length.ends_with('m'));
    assert!(s.next_event.starts_with("Set ") || s.next_event.starts_with("Rise "), "{}", s.next_event);
    assert!(s.sunrise.starts_with("Rise ") && s.sunset.starts_with("Set "), "{} {}", s.sunrise, s.sunset);
    assert_eq!(s.mode, "daylight");
    // June noon at 43 north (solar noon nearer one): the sun high, a little east of south.
    assert!(s.sun_elevation_deg > 50.0 && (120.0..210.0).contains(&s.sun_azimuth_deg), "{s:?}");
}

#[test]
fn before_the_clock_is_set_it_says_so() {
    let unset = TimeReading { valid: false, ..noon() };
    let st = status_at(unset, |_| {});
    let s = Screen::of(&st).view;
    assert_eq!(
        (s.time.as_str(), s.day_length.as_str(), s.next_event.as_str()),
        ("--:--", "Waiting for", "time sync...")
    );
    assert_eq!((s.sunrise.as_str(), s.sunset.as_str()), ("Rise --:--", "Set --:--"));
    assert_eq!(sky_of(&s), 0); // a night sky, and no sun
}

#[test]
fn the_mode_and_the_sky_in_their_colors() {
    let st = status(|h| h.post(ButtonPressed { press: Press::Double, at: h.now, ..ButtonPressed::default() }));
    let v = Screen::of(&st).view;
    assert_eq!((v.mode, v.mode_tone), ("RED ALERT", Tone::Warning));

    let st = status(|h| {
        h.post(ConditionsUpdated {
            now: Conditions { cloud_percent: 95, precip: Precip::Snow, ..Conditions::default() },
            at: h.now,
        });
    });
    assert_eq!(Screen::of(&st).view.sky, "snow");

    let st = status(|h| {
        let mut a = AlertsUpdated { at: h.now, ..AlertsUpdated::default() };
        a.alerts.push(Alert { level: AlertLevel::Warning, until: h.now + hours(1), ..Alert::default() });
        h.post(a);
    });
    let v = Screen::of(&st).view;
    assert_eq!((v.sky.as_str(), v.sky_tone), ("warning", Tone::Warning));

    let st = status(|h| h.post(CloudCoverUpdated { forecast: tuesday_forecast() }));
    assert_eq!(Screen::of(&st).view.sky, "cloud 20%");
}

#[test]
fn the_sky_follows_the_sun() {
    let sky_at = |time| sky_of(&Screen::of(&status_at(at(time), |_| {})).view);
    assert_eq!(sky_at(hours(12)), 3); // day
    assert_eq!(sky_at(hours(1)), 0); // night
    // The top band is the sky's color, where no text or sun is.
    let frame = drawn(&Screen::of(&status(|_| {})));
    assert_eq!(frame.at(Point::new(120, 22)), rgb(SKIES[3][1]));
}

#[test]
fn the_sun_rises_on_the_left_and_sets_on_the_right() {
    let morning = sun_at(&Screen::of(&status_at(at(hours(8)), |_| {})).view);
    let noon = sun_at(&Screen::of(&status(|_| {})).view);
    let evening = sun_at(&Screen::of(&status_at(at(hours(18)), |_| {})).view);
    assert!(morning.x < noon.x && noon.x < evening.x, "{morning:?} {noon:?} {evening:?}");
    assert!(noon.y < morning.y && noon.y < evening.y); // higher at noon
    // Its disc is drawn there.
    let frame = drawn(&Screen::of(&status(|_| {})));
    assert_eq!(frame.at(noon), rgb(0xffdf91));
}

#[test]
fn a_cloud_shows_when_its_cloudy_or_falling() {
    let cloud_at = Point::new(158, 70);
    let clear = drawn(&Screen::of(&status(|_| {})));
    let cloudy = drawn(&Screen::of(&status(|h| {
        h.post(ConditionsUpdated {
            now: Conditions { cloud_percent: 90, precip: Precip::Rain, ..Conditions::default() },
            at: h.now,
        });
    })));
    assert_ne!(clear.at(cloud_at), cloudy.at(cloud_at));
}

#[test]
fn a_dot_per_device_in_its_links_color() {
    let st = status(|h| {
        h.pair_bulb(WALL_A, 0xA1, 0x0001); // heard at pairing: online
        h.pair_bulb(WORK, 0xB2, 0x0002);
        h.tick(200); // Work's heard nothing since: silent (as is WallA, unless it acks)
        h.post(AckReceived { short_addr: 0x0001, rssi: -50, status: 0 });
    });
    let frame = drawn(&Screen::of(&st));
    assert_eq!(dot_color(&frame, WALL_A), ONLINE);
    assert_eq!(dot_color(&frame, WORK), SILENT);
    assert_eq!(dot_color(&frame, WALL_B), UNPAIRED);
    assert_eq!(dot_color(&frame, BUTTON), link_color(DeviceLink::Unpaired));
}

#[test]
fn the_picture_has_text_where_it_should_and_fits_the_panel() {
    let st = status(|h| h.post(CloudCoverUpdated { forecast: tuesday_forecast() }));
    let frame = drawn(&Screen::of(&st));
    let lit =
        |x0: i32, y0: i32, x1: i32, y1: i32| (y0..y1).any(|y| (x0..x1).any(|x| frame.at(Point::new(x, y)) != BLACK));
    let white = |x0: i32, y0: i32, x1: i32, y1: i32| {
        (y0..y1).any(|y| (x0..x1).any(|x| frame.at(Point::new(x, y)) == rgb(0xFFFFFF)))
    };
    assert!(white(6, 0, 120, 50)); // the time
    assert!(white(180, 4, 234, 26)); // the weekday
    assert!(white(6, 54, 120, 74)); // the day length
    assert!(white(6, 87, 80, 100)); // the sunrise
    assert!(white(160, 87, 234, 100)); // the sunset
    assert!(white(50, 104, 190, 117)); // the next event
    assert!(lit(8, 118, 200, 132)); // the dots

    // For a look at it: SUNSIM_FRAME=path.ppm cargo test --test display
    if let Ok(path) = std::env::var("SUNSIM_FRAME") {
        let mut ppm = format!("P6 {WIDTH} {HEIGHT} 255\n").into_bytes();
        for p in &frame.pixels {
            ppm.extend([p.r() << 3, p.g() << 2, p.b() << 3]);
        }
        std::fs::write(path, ppm).unwrap();
    }
}

#[test]
fn the_splash_says_sunsim() {
    let mut frame = Frame::default();
    draw_splash(&mut frame);
    assert!(frame.pixels.iter().any(|&p| p != BLACK));
}

#[test]
fn red_alert_shows_the_starship_and_its_bars_flash_on_the_beat() {
    let st = status(|h| h.post(ButtonPressed { press: Press::Double, at: h.now, ..ButtonPressed::default() }));
    let screen = Screen::of(&st);
    assert!(screen.view.red_alert());
    let frame = drawn(&screen);
    // Space, not the sky; the saucer and the glowing collector; the bars; the dots.
    assert_eq!(frame.at(Point::new(5, 70)), rgb(0x02040c));
    assert_eq!(frame.at(Point::new(150, 36)), rgb(0xc9ced6));
    assert_eq!(frame.at(Point::new(110, 47)), rgb(0xffb0a0));
    assert_eq!(dot_color(&frame, WALL_A), UNPAIRED);

    let lit = drawn(&Screen { beat: true, ..screen.clone() });
    let dim = drawn(&Screen { beat: false, ..screen.clone() });
    assert_ne!(lit.at(Point::new(230, 3)), dim.at(Point::new(230, 3)));

    // For a look at it: SUNSIM_FRAME=path.ppm cargo test --test display red_alert
    if let Ok(path) = std::env::var("SUNSIM_FRAME") {
        let mut ppm = format!("P6 {WIDTH} {HEIGHT} 255\n").into_bytes();
        for p in &lit.pixels {
            ppm.extend([p.r() << 3, p.g() << 2, p.b() << 3]);
        }
        std::fs::write(path, ppm).unwrap();
    }
}

#[test]
fn out_of_red_alert_the_sky_comes_back_and_nothing_beats() {
    let st = status(|_| {});
    let screen = Screen::of(&st);
    assert!(!screen.view.red_alert() && !screen.beat);
    assert_eq!(drawn(&screen).at(Point::new(120, 22)), rgb(SKIES[3][1]));
}

#[test]
fn a_fade_dims_the_picture_to_black_and_back() {
    let frame = drawn(&Screen::of(&status(|_| {})));
    let p = Point::new(120, 22);
    let at = |brightness: u8| frame.faded(brightness).nth((p.y as u32 * WIDTH + p.x as u32) as usize).unwrap();
    assert_eq!(at(255), frame.at(p));
    assert_eq!(at(0), BLACK);
    let half = at(128);
    assert!(half != BLACK && half != frame.at(p));
}

#[test]
fn nothing_of_red_alert_stays_when_the_sky_comes_back() {
    let alert =
        Screen::of(&status(|h| h.post(ButtonPressed { press: Press::Double, at: h.now, ..ButtonPressed::default() })));
    let sky = Screen::of(&status(|_| {}));
    let mut reused = Frame::default();
    alert.draw(&mut reused);
    sky.draw(&mut reused); // the board draws each screen over the last
    assert!(reused.pixels == drawn(&sky).pixels);
}
