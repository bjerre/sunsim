//! Pairing, discovery, liveness and the role layout: DeviceRegistry and LightOutput.

use ev::*;
use sunsim_core::commands_for;
use sunsim_tests::*;

#[test]
fn pairing_assigns_the_armed_role_persists_discovers_then_lights_it() {
    let mut h = Harness::new();
    h.tick(1); // the program needs a time to compute targets
    h.post(PairRequested { role: WALL_C });
    assert_eq!(h.all::<OpenNetwork>().len(), 1);

    h.post(DeviceAnnounced { ext_addr: 0xAA, short_addr: 0x1234 });
    let assigned = h.all::<RoleAssigned>();
    assert!(assigned.len() == 1 && assigned[0].role == WALL_C && assigned[0].record.ext_addr == 0xAA);
    assert!(!h.all::<PersistRoles>().is_empty());
    let discover = h.all::<Discover>();
    assert!(discover.len() == 1 && discover[0].role == WALL_C && discover[0].short_addr == 0x1234);

    h.clear_log();
    h.post(EndpointFound { role: WALL_C, endpoint: 11 });
    let lights = h.all::<SetLight>();
    assert!(lights.len() == 1 && lights[0].short_addr == 0x1234 && lights[0].endpoint == 11);
    assert_eq!(lights[0].power, Some(true)); // first command switches it on
}

#[test]
fn a_device_that_already_holds_a_role_is_never_claimed_by_pair() {
    let mut h = Harness::new();
    h.pair_bulb(WALL_A, 0xA1, 0x0001);
    h.post(PairRequested { role: WALL_C });
    h.clear_log();
    h.post(DeviceAnnounced { ext_addr: 0xA1, short_addr: 0x0002 }); // WallA power-cycles
    let assigned = h.all::<RoleAssigned>();
    assert!(assigned.len() == 1 && assigned[0].role == WALL_A && assigned[0].record.short_addr == 0x0002); // WallC not assigned
}

#[test]
fn unknown_devices_are_ignored_when_nothing_is_armed() {
    let mut h = Harness::new();
    h.post(DeviceAnnounced { ext_addr: 0xBEEF, short_addr: 0x0005 });
    assert!(h.all::<RoleAssigned>().is_empty());
    assert!(h.all::<PersistRoles>().is_empty());
}

#[test]
fn liveness_acks_online_150_s_of_silence_silent() {
    let mut h = Harness::new();
    h.tick(1);
    h.pair_bulb(WORK, 0xB2, 0x0042);
    h.post(AckReceived { short_addr: 0x0042, rssi: -48, status: 0 });
    h.tick(1);
    let snap = h.all::<StatusChanged>().last().unwrap().snapshot.clone();
    assert_eq!(snap.devices[WORK].link, DeviceLink::Online);
    assert_eq!(snap.devices[WORK].rssi, -48);

    h.clear_log();
    h.tick(160);
    let snap = h.all::<StatusChanged>().last().unwrap().snapshot.clone();
    assert_eq!(snap.devices[WORK].link, DeviceLink::Silent);
    assert!(h.all::<Ping>().len() >= 2); // pinged every 60 s meanwhile
}

#[test]
fn endpoint_discovery_is_retried_every_15_s_until_found() {
    let mut h = Harness::new();
    h.post(PairRequested { role: WALL_B });
    h.post(DeviceAnnounced { ext_addr: 0xC3, short_addr: 0x0003 });
    h.post(EndpointFound { role: WALL_B, endpoint: 0 }); // no reply
    h.clear_log();
    h.tick(30);
    assert_eq!(h.all::<Discover>().len(), 2);
}

#[test]
fn unpairing_a_role_mid_discovery_stops_its_retries() {
    let mut h = Harness::new();
    h.post(PairRequested { role: WALL_B });
    h.post(DeviceAnnounced { ext_addr: 0xC3, short_addr: 0x0003 }); // no EndpointFound
    h.post(UnpairRequested { roles: WALL_B.into() });
    h.clear_log();
    h.tick(45);
    assert!(h.all::<Discover>().is_empty());
    assert!(h.all::<Ping>().is_empty());
}

#[test]
fn a_ping_that_gets_no_answer_at_all_counts_as_failed() {
    let mut h = Harness::new();
    h.tick(1);
    h.pair_bulb(WORK, 0xB2, 0x0002);
    h.clear_log();
    h.tick(90); // pinged at 15 s and 75 s, never answered; silent since pairing
    let changes = h.all::<ReachabilityChanged>();
    assert!(changes.len() == 1 && changes[0].role == WORK && !changes[0].reachable);
}

#[test]
fn button_pairing_binds_it_once_its_endpoint_is_found() {
    let mut h = Harness::new();
    h.post(PairRequested { role: BUTTON });
    h.post(DeviceAnnounced { ext_addr: 0x18690AFFFE12620E, short_addr: 0x2FED });
    h.clear_log();
    h.post(EndpointFound { role: BUTTON, endpoint: 1 });
    let binds = h.all::<Bind>();
    assert!(binds.len() == 1 && binds[0].short_addr == 0x2FED && binds[0].endpoint == 1);
    assert!(h.all::<SetLight>().is_empty()); // never treated as a bulb
}

#[test]
fn a_missing_bulb_is_paused_the_others_keep_going_and_it_resumes_when_it_answers() {
    let mut h = Harness::new();
    let ack = |h: &mut Harness| h.post(AckReceived { short_addr: 0x0001, rssi: -50, status: 0 });
    h.tick(1);
    h.pair_bulb(WALL_A, 0xA1, 0x0001);
    h.pair_bulb(WORK, 0xB2, 0x0002); // never answers
    ack(&mut h);
    h.clear_log();

    h.tick(14);
    ack(&mut h);
    h.tick(1); // 15 s after pairing: WallA's first ping
    let pings = h.all::<Ping>();
    assert!(pings.len() == 1 && pings[0].role == WALL_A);
    // WallA's ping is lost too, but it acked recently: still reachable.
    h.post(PingResult { role: WALL_A, ok: false, ..PingResult::default() });

    h.tick(42); // Work's turn: slot 5 of 7, 5/7 of the minute later
    let pings = h.all::<Ping>();
    assert!(pings.len() == 2 && pings[1].role == WORK);
    // Work's ping fails and it was never heard after pairing... except
    // at pairing itself, so it needs 60 s of silence first.
    h.post(PingResult { role: WORK, ok: false, ..PingResult::default() });
    assert!(h.all::<ReachabilityChanged>().is_empty());

    h.tick(30);
    ack(&mut h); // WallA keeps answering
    h.tick(30); // Work's next ping
    ack(&mut h);
    h.post(PingResult { role: WORK, ok: false, ..PingResult::default() });
    let changes = h.all::<ReachabilityChanged>();
    assert!(changes.len() == 1 && changes[0].role == WORK && !changes[0].reachable);

    h.clear_log();
    let later = at(hours(14)); // the curve has moved on since the last update
    h.post(ProgramTick { time: later, transition: Transition::secs(60) });
    let sent = h.all::<SetLight>();
    assert!(!sent.iter().any(|l| l.short_addr == 0x0002)); // paused
    assert!(sent.iter().any(|l| l.short_addr == 0x0001)); // others unaffected (level/color moved since last time)

    h.clear_log();
    h.tick(15);
    assert!(!h.all::<Ping>().is_empty()); // unreachable bulbs are checked every 15 s

    h.clear_log();
    h.post(PingResult { role: WORK, ok: true, ..PingResult::default() }); // powered back on
    let changes = h.all::<ReachabilityChanged>();
    assert!(changes.len() == 1 && changes[0].reachable);
    // Re-applied in full at once.
    assert!(h.all::<SetLight>().iter().any(|l| l.short_addr == 0x0002 && l.power == Some(true)));
}

#[test]
fn only_changed_values_are_re_sent() {
    let mut h = Harness::new();
    h.tick(1);
    h.pair_bulb(WALL_B, 0xC3, 0x0003);
    h.post(ProgramTick { time: noon(), transition: Transition::secs(60) });
    h.clear_log();
    h.post(ProgramTick { time: noon(), transition: Transition::secs(60) }); // same time -> same target
    assert!(h.all::<SetLight>().is_empty());
}

#[test]
fn unpair_all_clears_every_role_and_persists() {
    let mut h = Harness::new();
    h.pair_bulb(WALL_A, 0xA1, 0x0001);
    h.clear_log();
    h.post(UnpairRequested::default());
    assert_eq!(h.all::<RoleCleared>().len(), ROLE_COUNT);
    let persisted = h.all::<PersistRoles>();
    assert!(persisted.len() == 1 && !persisted[0].records[WALL_A].valid);
}

#[test]
fn the_layout_five_wall_bulbs_the_work_light_two_schedule_plugs_and_the_button() {
    const _: () = assert!(ROLE_COUNT == 9 && LIGHT_COUNT == 8);
    const _: () = assert!(slot(WORK) == 5 && slot(DESK) == 6 && slot(GROW) == 7 && slot(BUTTON) == 8);
    const _: () = assert!(slot(ROLES[slot(WALL_C)]) == slot(WALL_C));
    const _: () = assert!(matches!(kind_of(DESK), Kind::Plug) && matches!(kind_of(GROW), Kind::Plug));
    const _: () = assert!(matches!(parse_role("wallc"), Some(Role { group: Group::Wall, index: 2 })));
    const _: () = assert!(parse_role("WallF").is_none());
    const _: () = assert!(matches!(kind_of(WALL_A), Kind::ColorBulb) && matches!(kind_of(BUTTON), Kind::Button));
    const _: () = assert!(is_light(WORK) && !is_light(BUTTON) && is_button(BUTTON) && !is_button(WORK));
    assert_eq!(members(Group::Wall).len(), 5);
    assert!(role_name(WALL_A) == "WallA" && role_name(WALL_E) == "WallE");
    assert!(role_name(WORK) == "Work" && role_name(BUTTON) == "Button");
    assert!(role_name(DESK) == "Desk" && role_name(GROW) == "Grow");
    assert!(lights().eq(ROLES[..8].iter().copied()));
    let desk = parse_selection("desk").unwrap();
    assert!(selects(desk, DESK) && !selects(desk, GROW));

    let wall = parse_selection("wall").unwrap();
    assert!(selects(wall, WALL_E) && !selects(wall, WORK));
    let work = parse_selection("WORK").unwrap();
    assert!(selects(work, WORK) && !selects(work, WALL_A));
    let all = parse_selection("all").unwrap();
    assert!(ROLES.iter().all(|&r| selects(all, r)));
    assert!(parse_selection("lamp").is_none());
}

#[test]
fn unpairing_a_group_clears_just_its_roles() {
    let mut h = Harness::new();
    h.pair_bulb(WALL_A, 0xA1, 0x0001);
    h.pair_bulb(WALL_E, 0xA5, 0x0005);
    h.pair_bulb(WORK, 0xB2, 0x0002);
    h.clear_log();
    h.post(UnpairRequested { roles: Group::Wall.into() });
    assert_eq!(h.all::<RoleCleared>().len(), members(Group::Wall).len());
    let persisted = h.all::<PersistRoles>();
    assert_eq!(persisted.len(), 1);
    let records = &persisted[0].records;
    assert!(!records[WALL_A].valid && !records[WALL_E].valid && records[WORK].valid);
}

#[test]
fn addresses_are_checked_against_the_layout_by_name_a_known_one_joins_as_its_role() {
    const ADDRESSES: Addresses = addresses_of(&[
        Address { role: "WallE", ext_addr: 0xE5 },
        Address { role: "work", ext_addr: 0xB2 },
        Address { role: "WallB", ext_addr: 0 },
    ]);
    const _: () = assert!(*ADDRESSES.get(WALL_E) == 0xE5 && *ADDRESSES.get(WORK) == 0xB2);
    const _: () = assert!(*ADDRESSES.get(WALL_A) == 0 && *ADDRESSES.get(WALL_B) == 0); // 0: none given
    // Wrong names, a role listed twice or an address given twice don't compile.

    let mut h = Harness::with_addresses(ADDRESSES);
    h.post(DeviceAnnounced { ext_addr: 0xE5, short_addr: 0x0005 });
    let assigned = h.all::<RoleAssigned>();
    assert!(assigned.len() == 1 && assigned[0].role == WALL_E);
}

#[test]
fn each_kind_of_light_gets_only_the_commands_it_takes() {
    let amber = LightState { on: true, level: 80, color: Xy { x: 0.55, y: 0.41 }.into() };
    let red = LightState { on: true, level: 200, color: HueSat { hue: 0, saturation: 254 }.into() };
    let to = |state| LightChange {
        state,
        level_transition: Transition::secs(60),
        color_transition: Transition::secs(60),
        ..LightChange::default()
    };

    // A color bulb: everything, then only what moves.
    let color = commands_for(None, &to(amber), Kind::ColorBulb.caps()).unwrap();
    assert!(color.power == Some(true) && color.level == Some(80) && xy(color.color).is_some());
    let dimmer = commands_for(Some(&amber), &to(LightState { level: 60, ..amber }), Kind::ColorBulb.caps()).unwrap();
    assert!(dimmer.power.is_none() && dimmer.level == Some(60) && dimmer.color.is_none());

    // A plug: only on and off.
    let on = commands_for(None, &to(amber), Kind::Plug.caps()).unwrap();
    assert!(on.power == Some(true) && on.level.is_none() && on.color.is_none());
    assert!(commands_for(Some(&amber), &to(red), Kind::Plug.caps()).is_none()); // still on: nothing to send
    let off = commands_for(Some(&amber), &to(LightState::default()), Kind::Plug.caps()).unwrap();
    assert_eq!(off.power, Some(false));
}

#[test]
fn zones_every_light_in_one_with_its_look_buttons_control_theirs() {
    assert!(
        Installation::built_in().zones.len() == 3
            && TOPOLOGY.zone_of[WALL_A] == Some(ZoneId(0))
            && TOPOLOGY.zone_of[BUTTON] == Some(ZoneId(0))
    );
    assert!(TOPOLOGY.zone_of[DESK] == Some(ZoneId(1)) && TOPOLOGY.zone_of[GROW] == Some(ZoneId(2)));
    assert!(matches!(TOPOLOGY.look_of[DESK], Look::OnOffOnly) && matches!(TOPOLOGY.look_of[GROW], Look::OnOffOnly));
    assert!(DEFAULT_PEAKS[WORK] == 90 && DEFAULT_PEAKS[WALL_A] == 100 && DEFAULT_PEAKS[DESK] == 100);
    assert!(TOPOLOGY.controls_of[BUTTON].is_some() && TOPOLOGY.controls_of[WORK].is_none());
    assert!(TOPOLOGY.weather.precip.contains(WALL_C) && TOPOLOGY.weather.alerts.contains(WALL_B));
    assert!(!TOPOLOGY.weather.alerts.contains(WALL_C));

    // Another room: the wall in one zone; the work light, the plugs and the button in another.
    const ZONES: [ZoneSpec; 2] = [
        ZoneSpec {
            name: "wall",
            lights: &[Shows::new("wall", Look::SunCurve)],
            weather: Some(Weather { precip: &["wall"], alerts: &[] }),
            ..ZoneSpec::DEFAULT
        },
        ZoneSpec {
            name: "desk",
            lights: &[
                Shows::new("WORK", Look::WorkCurve(WorkCurve::SETTINGS)),
                Shows::new("Desk", Look::OnOffOnly),
                Shows::new("Grow", Look::OnOffOnly),
            ],
            modes: &[Mode::Daylight, Mode::Off],
            controls: &[Controls { twice: None, ..Controls::by("button") }],
            ..ZoneSpec::DEFAULT
        },
    ];
    const T: Topology = topology_of(&ZONES);
    assert!(
        T.zone_of[WALL_E] == Some(ZoneId(0))
            && T.zone_of[WORK] == Some(ZoneId(1))
            && T.zone_of[BUTTON] == Some(ZoneId(1))
    );
    assert!(matches!(T.look_of[WORK], Look::WorkCurve(_)) && matches!(T.look_of[WALL_A], Look::SunCurve));
    let controls = T.controls_of[BUTTON].unwrap();
    assert!(controls.twice.is_none() && controls.hold == Some(Mode::Off));
    assert!(ZONES[1].allows(Mode::Off) && !ZONES[1].allows(Mode::RedAlert));
    assert!(T.weather.precip.len() == 5 && T.weather.precip.contains(WALL_E) && T.weather.alerts.is_empty());
    // A light in no zone or two, a look on the button, a button picking red alert in "desk", ... don't compile.

    // A gate's rules after one that always answers are never asked, so that doesn't compile either.
    const NINE_TO_FIVE: Window = Window::hours(Tunable::Fixed(DaySpan::new(hours(9), hours(17))));
    assert!(answers_always(&Rule::ALWAYS) && answers_always(&NINE_TO_FIVE.rule()));
    assert!(!answers_always(&NINE_TO_FIVE.closes_only()) && !answers_always(&NINE_TO_FIVE.opens_only()));
    const LAYERED: Topology = topology_of(&[ZoneSpec {
        name: "all",
        lights: &[
            Shows::new("wall", Look::SunCurve),
            Shows::new("work", Look::WorkCurve(WorkCurve::SETTINGS)),
            Shows::new("Desk", Look::OnOffOnly),
            Shows::new("Grow", Look::OnOffOnly),
        ],
        gate: &[NINE_TO_FIVE.closes_only(), NINE_TO_FIVE.opens_only(), Rule::ALWAYS],
        ..ZoneSpec::DEFAULT
    }]);
    assert_eq!(LAYERED.zone_of[WORK], Some(ZoneId(0))); // one-way rules first: fine
}

#[test]
fn the_desk_and_grow_plugs_follow_their_own_daily_schedules() {
    let mut h = Harness::new();
    h.tick(1);
    let is_on = |h: &mut Harness, role: Role, time: Duration| {
        h.clear_log();
        h.post(ProgramTick { time: at(time), transition: Transition::secs(60) });
        h.all::<LightTarget>().into_iter().find(|t| t.role == role).is_some_and(|t| t.state.on)
    };
    assert!(!is_on(&mut h, DESK, hours(7) + mins(59)));
    assert!(is_on(&mut h, DESK, hours(8)));
    assert!(is_on(&mut h, DESK, hours(18) + mins(59)));
    assert!(!is_on(&mut h, DESK, hours(19)));
    assert!(!is_on(&mut h, GROW, hours(3) + mins(44)));
    assert!(is_on(&mut h, GROW, hours(3) + mins(45)));
    assert!(is_on(&mut h, GROW, hours(8) + mins(44)));
    assert!(!is_on(&mut h, GROW, hours(8) + mins(45)));

    // The button controls the office alone: the plugs keep their schedules
    // while its program is off.
    h.post(ButtonPressed { press: Press::Hold, at: h.now, ..ButtonPressed::default() });
    assert!(is_on(&mut h, DESK, hours(12)));
}

#[test]
fn a_plug_gets_on_and_off_alone() {
    let mut h = Harness::new();
    h.tick(1);
    h.pair_bulb(DESK, 0xD1, 0x00D1);
    let to_desk = h.all::<SetLight>().into_iter().filter(|s| s.role == DESK).collect::<Vec<_>>();
    assert!(
        to_desk.len() == 1
            && to_desk[0].power == Some(true)
            && to_desk[0].level.is_none()
            && to_desk[0].color.is_none()
    );
}
