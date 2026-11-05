//! The event vocabulary: the bus, the settings table.

use std::cell::RefCell;
use std::rc::Rc;

use ev::*;
use sunsim_tests::*;

#[test]
fn the_bus_delivers_every_event_posted_from_many_threads_in_each_posters_order() {
    let mut bus = EventBus::new();
    let seen = Rc::new(RefCell::new([Vec::new(), Vec::new()]));
    let recorder = seen.clone();
    bus.on::<AckReceived>(move |a| recorder.borrow_mut()[a.status as usize].push(a.short_addr));
    const PER_THREAD: usize = 20000;
    let posters: Vec<_> = (0..2u8)
        .map(|t| {
            let post = bus.post().clone();
            std::thread::spawn(move || {
                for i in 0..PER_THREAD {
                    post.send(AckReceived { short_addr: i as u16, rssi: 0, status: t });
                }
            })
        })
        .collect();
    while seen.borrow().iter().map(Vec::len).sum::<usize>() < 2 * PER_THREAD {
        bus.wait();
        bus.process();
    }
    for p in posters {
        p.join().unwrap();
    }
    for s in seen.borrow().iter() {
        assert_eq!(s.len(), PER_THREAD);
        assert!(s.is_sorted());
    }
}

#[test]
fn events_go_by_their_payloads_names() {
    assert_eq!(Event::from(OutputTarget::default()).name(), "OutputTarget");
    assert_eq!(Event::from(Tick::default()).name(), "Tick");
}

#[test]
fn settings_typed_shown_and_stored_from_one_table() {
    let mut c = ProgramConfig::default();
    let is = |c: &ProgramConfig, name: &str, text: &str| find_setting(name).unwrap().show(c) == text;
    let takes = |c: &mut ProgramConfig, name: &str, text: &str| find_setting(name).unwrap().set(c, text).is_ok();
    assert!(is(&c, "hours", "08:00-19:00") && is(&c, "days", "mon-fri") && is(&c, "fade", "10 min"));
    assert!(is(&c, "sunshift", "25%") && is(&c, "ambersweep", "3 deg") && is(&c, "amber", "on"));

    assert!(takes(&mut c, "hours", "07:30-24:00") && c.active == DaySpan::new(hours(7) + mins(30), hours(24)));
    assert!(takes(&mut c, "days", "sat,mon") && is(&c, "days", "mon,sat"));
    assert!(takes(&mut c, "clouds", "off") && !c.clouds_enabled);

    let before = c;
    assert!(!takes(&mut c, "hours", "19:00-08:00")); // backwards
    assert!(!takes(&mut c, "hours", "8-19"));
    assert!(!takes(&mut c, "sunshift", "101"));
    assert!(!takes(&mut c, "sunshift", ""));
    assert!(!takes(&mut c, "ambersweep", "31"));
    assert!(!takes(&mut c, "fade", "-1"));
    assert!(!takes(&mut c, "amber", "yes"));
    assert!(!takes(&mut c, "days", "mon,funday"));
    assert_eq!(before, c); // nothing changed
    assert!(find_setting("precip").is_none() && find_setting("alerts").is_none()); // declared in the zones instead

    assert_eq!(find_setting("sunshift").unwrap().set(&mut c, "lots"), Err("sunshift takes 0-100".to_string()));
    assert!(find_setting("color").is_none());

    // The keys the device already has values under.
    let keys = setting_keys();
    for key in [
        "act.start",
        "act.end",
        "act.fade",
        "act.days",
        "work.start",
        "work.end",
        "sun.shift",
        "sun.amber",
        "sun.sweep",
        "sky.on",
        "sky.dim",
        "sky.cool",
        "sync.start",
        "sync.end",
        "wx.on",
    ] {
        assert!(keys.contains(&key), "{key}");
    }
    assert_eq!(keys.len(), 15);
}
