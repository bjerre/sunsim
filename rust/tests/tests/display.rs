//! The status display's picture, from real core status: what it says, and
//! the pixels it draws.

use std::cell::RefCell;
use std::rc::Rc;

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use ev::*;
use platform::StatusSlot;
use platform::display::{
    BLACK, Frame, HEIGHT, ONLINE, RED, SILENT, Screen, UNPAIRED, WIDTH, dot_at, draw_splash, link_color,
};
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
    h.tick_at(1, time);
    setup(&mut h);
    h.tick_at(2, time);
    slot.borrow().get().expect("a status")
}

fn drawn(screen: &Screen) -> Frame {
    let mut frame = Frame::default();
    screen.draw(&mut frame).unwrap();
    frame
}

// The center of a device's dot.
fn dot_color(frame: &Frame, role: Role) -> Rgb565 {
    frame.at(dot_at(role) + Point::new(5, 5))
}

#[test]
fn the_time_the_day_and_the_next_sunrise_or_sunset() {
    let st = status(|_| {});
    let s = Screen::of(&st);
    assert_eq!(s.time, "12:00");
    assert_eq!(s.weekday, "Tue");
    assert!(s.day_length.starts_with("Day ") && s.day_length.ends_with('m'));
    assert!(s.next_event.starts_with("Set ") || s.next_event.starts_with("Rise "), "{}", s.next_event);
    assert_eq!(s.mode.0, "daylight");
}

#[test]
fn before_the_clock_is_set_it_says_so() {
    let unset = TimeReading { valid: false, ..noon() };
    let st = status_at(unset, |_| {});
    let s = Screen::of(&st);
    assert_eq!(
        (s.time.as_str(), s.day_length.as_str(), s.next_event.as_str()),
        ("--:--", "Waiting for", "time sync...")
    );
}

#[test]
fn the_mode_and_the_sky_in_their_colors() {
    let st = status(|h| h.post(ButtonPressed { press: Press::Double, at: h.now, ..ButtonPressed::default() }));
    assert_eq!(Screen::of(&st).mode, ("RED ALERT", RED));

    let st = status(|h| {
        h.post(ConditionsUpdated {
            now: Conditions { cloud_percent: 95, precip: Precip::Snow, ..Conditions::default() },
            at: h.now,
        });
    });
    assert_eq!(Screen::of(&st).sky.0, "snow");

    let st = status(|h| {
        let mut a = AlertsUpdated { at: h.now, ..AlertsUpdated::default() };
        a.alerts.push(Alert { level: AlertLevel::Warning, until: h.now + hours(1), ..Alert::default() });
        h.post(a);
    });
    assert_eq!(Screen::of(&st).sky, ("warning".to_string(), RED));

    let st = status(|h| h.post(CloudCoverUpdated { forecast: tuesday_forecast() }));
    assert_eq!(Screen::of(&st).sky.0, "cloud 20%");
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
    assert!(lit(6, 0, 120, 50)); // the time
    assert!(lit(180, 6, 234, 26)); // the weekday
    assert!(lit(6, 54, 120, 74)); // the day length
    assert!(lit(6, 80, 200, 100)); // the next event
    assert!(lit(150, 58, 234, 72)); // the sky
    assert!(!lit(0, 132, WIDTH as i32, HEIGHT as i32)); // nothing runs off the bottom

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
    draw_splash(&mut frame).unwrap();
    assert!(frame.pixels.iter().any(|&p| p != BLACK));
}
