//! The paced send queue: SetLight in, one ZCL command per slot out, to a
//! Zigbee group where every light in it waits for the same command.

use std::sync::{Arc, Mutex};

use ev::*;
use sunsim_core::{LightGroup, SendQueue};
use sunsim_tests::*;

fn set_of(roles: &[Role]) -> RoleSet {
    roles.iter().copied().collect()
}

// The groups the bulbs would be in for this room: the whole room, the wall,
// the alert lights, and the rest of the wall and the room without them.
const ROOM_GROUP: u16 = 0x5301;
const WALL_GROUP: u16 = 0x5302;
const ALERT_GROUP: u16 = 0x5303;
fn room_groups() -> Vec<LightGroup> {
    vec![
        LightGroup { id: ROOM_GROUP, members: set_of(&[WALL_A, WALL_B, WALL_C, WALL_D, WALL_E, WORK]) },
        LightGroup { id: WALL_GROUP, members: set_of(&[WALL_A, WALL_B, WALL_C, WALL_D, WALL_E]) },
        LightGroup { id: ALERT_GROUP, members: set_of(&[WALL_B, WALL_D]) },
        LightGroup { id: 0x5304, members: set_of(&[WALL_A, WALL_C, WALL_E]) },
        LightGroup { id: 0x5305, members: set_of(&[WALL_A, WALL_C, WALL_E, WORK]) },
    ]
}

// Each light's short address in the harness: its slot, plus one.
fn address_of(role: Role) -> u16 {
    slot(role) as u16 + 1
}

#[derive(Default)]
struct Out {
    sent: Vec<SendLight>,
    waiting: bool, // a SendsWaiting not yet answered
}

// A SendQueue on its own, given its slots by hand.
struct Sends {
    out: Arc<Mutex<Out>>,
    now: Instant,
    queue: SendQueue,
}

impl Sends {
    fn new(groups: Vec<LightGroup>) -> Self {
        let out = Arc::new(Mutex::new(Out::default()));
        let recorder = out.clone();
        let post = Post::new(move |e| {
            let mut out = recorder.lock().unwrap();
            match e {
                Event::SendLight(s) => out.sent.push(s),
                Event::SendsWaiting(_) => out.waiting = true,
                _ => {}
            }
        });
        Sends { out, now: Instant::ZERO, queue: SendQueue::new(post, groups) }
    }

    fn waiting(&self) -> bool {
        self.out.lock().unwrap().waiting
    }
    fn sent(&self) -> Vec<SendLight> {
        self.out.lock().unwrap().sent.clone()
    }
    fn clear(&self) {
        self.out.lock().unwrap().sent.clear();
    }

    // Slots, 25 ms apart, until nothing's waiting.
    fn drain(&mut self) {
        while std::mem::take(&mut self.out.lock().unwrap().waiting) {
            self.now += ms(25);
            self.queue.send_slot(&SendSlot { at: self.now });
        }
    }

    // What the core posted, through the queue.
    fn take(&mut self, h: &Harness) {
        for s in h.all::<SetLight>() {
            self.queue.set_light(&s);
        }
        self.drain();
    }

    fn to_group(&self, group: u16) -> Vec<SendLight> {
        self.sent().into_iter().filter(|s| s.to.group && s.to.address == group).collect()
    }
    fn to_light(&self, light: Role) -> Vec<SendLight> {
        self.sent().into_iter().filter(|s| !s.to.group && s.to.address == address_of(light)).collect()
    }
}

fn set_light(role: Role, power: Option<bool>, level: Option<u8>, color: Option<Color>) -> SetLight {
    SetLight { role, short_addr: address_of(role), endpoint: 11, power, level, color, ..SetLight::default() }
}

// Every light paired, each at address_of(), the daylight program running.
fn pair_room(h: &mut Harness) {
    h.tick(1);
    for role in lights() {
        h.pair_bulb(role, 0xA0 + slot(role) as u64, address_of(role));
    }
    h.clear_log();
}

const RED: Color = Color::HueSat(HueSat { hue: 0, saturation: 254 });

#[test]
fn commands_wait_for_a_slot_each_a_lights_keep_their_order_the_lights_take_turns() {
    let mut s = Sends::new(Vec::new());
    s.queue.set_light(&set_light(WALL_A, Some(true), Some(200), Some(RED)));
    s.queue.set_light(&set_light(WALL_B, None, Some(100), None));
    assert!(s.waiting() && s.sent().is_empty()); // nothing until the first slot
    s.drain();
    let sent = s.sent();
    assert_eq!(sent.len(), 4);
    assert!(sent[0].to.address == address_of(WALL_A) && sent[0].power == Some(true));
    assert!(sent[1].to.address == address_of(WALL_B) && sent[1].level == Some(100));
    assert!(sent[2].to.address == address_of(WALL_A) && sent[2].color == Some(RED));
    assert!(sent[3].to.address == address_of(WALL_A) && sent[3].level == Some(200));
    assert!(sent[0].backlog == 2 && sent[3].backlog == 1);
    assert!(!s.waiting());
}

#[test]
fn a_whiter_color_goes_after_the_level_a_saturated_one_before_it() {
    let mut s = Sends::new(Vec::new());
    s.queue.set_light(&set_light(WALL_A, None, Some(50), Some(Mireds(300).into())));
    s.drain();
    let sent = s.sent();
    assert!(sent.len() == 2 && sent[0].level.is_some() && sent[1].color.is_some());
}

#[test]
fn a_newer_command_merges_over_the_waiting_one_off_replaces_it() {
    let mut s = Sends::new(Vec::new());
    s.queue.set_light(&set_light(WALL_A, None, Some(100), None));
    s.queue.set_light(&set_light(WALL_A, None, Some(200), Some(Mireds(250).into())));
    s.drain();
    let sent = s.sent();
    assert!(sent.len() == 2 && sent[0].level == Some(200) && sent[1].color == Some(Mireds(250).into()));

    s.clear();
    s.queue.set_light(&set_light(WALL_A, Some(true), Some(100), None));
    s.queue.set_light(&set_light(WALL_A, Some(false), None, None));
    s.drain();
    let sent = s.sent();
    assert!(sent.len() == 1 && sent[0].power == Some(false));
}

#[test]
fn a_light_out_of_reach_or_unpaired_loses_what_was_waiting_for_it() {
    let mut s = Sends::new(Vec::new());
    for role in [WALL_A, WALL_B, WORK] {
        s.queue.set_light(&set_light(role, None, Some(100), None));
    }
    s.queue.on(&ReachabilityChanged { role: WALL_A, reachable: false }.into());
    s.queue.on(&RoleCleared { role: WORK }.into());
    s.drain();
    let sent = s.sent();
    assert!(sent.len() == 1 && sent[0].to.address == address_of(WALL_B));
}

#[test]
fn red_alert_the_whole_room_in_one_group_command_per_step() {
    let mut h = Harness::new();
    pair_room(&mut h);
    h.post(ButtonPressed { press: Press::Double, at: h.now, ..ButtonPressed::default() });
    let mut s = Sends::new(room_groups());
    s.take(&h);
    let sent = s.sent();
    assert_eq!(sent.len(), 2); // the red, then the level
    assert_eq!(s.to_group(ROOM_GROUP).len(), 2);
    assert!(sent[0].color == Some(RED) && sent[1].level.is_some());

    h.clear_log();
    s.clear();
    h.tick(1); // the beat: only the level moves
    s.take(&h);
    let sent = s.sent();
    assert!(sent.len() == 1 && s.to_group(ROOM_GROUP).len() == 1 && sent[0].level.is_some());
}

#[test]
fn red_alert_with_a_light_out_of_reach_the_largest_groups_without_it() {
    let mut h = Harness::new();
    pair_room(&mut h);
    h.post(ReachabilityChanged { role: WALL_C, reachable: false });
    h.post(ButtonPressed { press: Press::Double, at: h.now, ..ButtonPressed::default() });
    let mut s = Sends::new(room_groups());
    s.queue.reachability_changed(&ReachabilityChanged { role: WALL_C, reachable: false });
    s.take(&h);
    assert!(s.to_group(ROOM_GROUP).is_empty() && s.to_group(WALL_GROUP).is_empty() && s.to_light(WALL_C).is_empty());
    assert_eq!(s.to_group(ALERT_GROUP).len(), 2); // B and D together
    for role in [WALL_A, WALL_E, WORK] {
        assert_eq!(s.to_light(role).len(), 2);
    }
}

#[test]
fn a_weather_alert_its_two_lights_share_a_group_the_rest_go_on_with_the_sun() {
    let mut h = Harness::new();
    pair_room(&mut h);
    h.post(ProgramTick { time: noon(), transition: Transition::secs(60) });
    let mut watch = AlertsUpdated { at: h.now, ..AlertsUpdated::default() };
    let mut alert = Alert { level: AlertLevel::Watch, until: h.now + hours(2), ..Alert::default() };
    alert.set_event("X");
    watch.alerts.push(alert);
    h.post(watch);
    let mut s = Sends::new(room_groups());
    s.take(&h);
    s.clear();
    h.clear_log();
    h.tick(3); // the pulse fades, and the program goes on
    h.post(ProgramTick { time: noon(), transition: Transition::secs(60) });
    s.take(&h);
    assert!(!s.to_group(ALERT_GROUP).is_empty());
    assert!(s.to_light(WALL_B).is_empty() && s.to_light(WALL_D).is_empty()); // never one at a time
    assert!(s.to_group(ROOM_GROUP).is_empty() && s.to_group(WALL_GROUP).is_empty()); // never over the pulse
    for send in s.to_group(ALERT_GROUP) {
        assert!(send.color.is_none_or(|c| c.hue_sat().is_some())); // the watch's orange
    }
}

#[test]
fn the_lights_that_dont_share_a_group_get_theirs_one_at_a_time() {
    let mut s = Sends::new(room_groups());
    s.queue.set_light(&set_light(WALL_A, None, Some(100), None));
    s.queue.set_light(&set_light(WALL_B, None, Some(100), None));
    s.queue.set_light(&set_light(WALL_C, None, Some(120), None));
    s.drain(); // A and B want the same, but no group is just the two
    let sent = s.sent();
    assert!(sent.len() == 3 && sent.iter().all(|x| !x.to.group));
}

#[test]
fn past_the_broadcast_budget_group_commands_go_to_each_light() {
    let mut s = Sends::new(room_groups());
    for i in 0..SendQueue::MAX_GROUP_SENDS as u8 + 1 {
        for role in [WALL_B, WALL_D] {
            s.queue.set_light(&set_light(role, None, Some(10 + i), None));
        }
        s.drain();
    }
    assert_eq!(s.to_group(ALERT_GROUP).len(), SendQueue::MAX_GROUP_SENDS);
    assert!(s.to_light(WALL_B).len() == 1 && s.to_light(WALL_D).len() == 1);

    s.clear();
    s.now += SendQueue::GROUP_SEND_WINDOW;
    s.queue.set_light(&set_light(WALL_B, None, Some(1), None));
    s.queue.set_light(&set_light(WALL_D, None, Some(1), None));
    s.drain(); // the window's passed
    assert!(s.sent().len() == 1 && s.to_group(ALERT_GROUP).len() == 1);
}
