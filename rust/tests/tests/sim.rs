//! The whole app on simulated hardware (sim): the core with a virtual clock
//! and a Zigbee network of virtual bulbs and a button, run in virtual time
//! from joining on.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use ev::*;
use sim::{Bulb, Device, World};
use sunsim_core::LightGroup;
use sunsim_tests::*;

// Each device's IEEE address: in installation.rs's place, so each takes its
// role as it joins.
fn ext_of(role: Role) -> u64 {
    0x00124B0000000000 + slot(role) as u64
}

fn room_addresses() -> Addresses {
    let mut addresses = PerRole::filled(0);
    for role in ROLES {
        addresses[role] = ext_of(role);
    }
    addresses
}

const ROOM_GROUP: u16 = 0x5301;
fn room_group() -> Vec<LightGroup> {
    vec![LightGroup { id: ROOM_GROUP, members: lights().collect() }]
}

const RED: Color = Color::HueSat(HueSat { hue: 0, saturation: 254 });

// The room's six lights and its button, powered on at noon; the bulbs in
// `groups`, if any, are in those groups too.
struct Room {
    world: World,
    status: Rc<RefCell<Option<Arc<StatusSnapshot>>>>,
}

impl Room {
    fn new(groups: Vec<LightGroup>) -> Self {
        let mut world = World::new(noon(), room_addresses(), groups.clone());
        let status = Rc::new(RefCell::new(None));
        let latest = status.clone();
        world.bus.on::<StatusChanged>(move |e| *latest.borrow_mut() = Some(e.snapshot.clone()));
        {
            let mut network = world.network.borrow_mut();
            for role in ROLES {
                network.add(Device::new(ext_of(role), kind_of(role)));
                for g in groups.iter().filter(|g| g.members.contains(role)) {
                    network.join_group(ext_of(role), g.id);
                }
            }
        }
        world.start();
        Room { world, status }
    }

    fn bulb(&self, role: Role) -> Bulb {
        *self.world.network.borrow().bulb(ext_of(role)).unwrap()
    }
    fn link(&self, role: Role) -> DeviceLink {
        self.status.borrow().as_ref().map_or(DeviceLink::Unpaired, |s| s.devices[role].link)
    }
    fn press(&self, press: Press) {
        self.world.network.borrow_mut().press(ext_of(BUTTON), press);
    }
}

#[test]
fn a_simulated_room_joins_takes_its_roles_and_lights_up_with_the_sun() {
    let mut r = Room::new(Vec::new());
    r.world.run_for(secs(5));
    for light in lights() {
        assert_eq!(r.link(light), DeviceLink::Online);
        let b = r.bulb(light);
        assert!(b.on && b.to > 0 && b.color.is_some());
    }
    assert_ne!(r.link(BUTTON), DeviceLink::Unpaired);
    assert_eq!(r.world.network.borrow().counts().lost, 0);
}

#[test]
fn virtual_time_an_hour_runs_at_once_and_the_clock_keeps_up() {
    let mut r = Room::new(Vec::new());
    r.world.run_for(hours(1));
    assert_eq!(r.status.borrow().as_ref().unwrap().time.local, noon().local + hours(1));
}

#[test]
fn a_double_press_turns_the_room_red_one_group_command_per_step() {
    let mut r = Room::new(room_group());
    r.world.run_for(secs(5));
    let before = r.world.network.borrow().counts();
    r.press(Press::Double);
    r.world.run_for(ms(300));
    let after = r.world.network.borrow().counts();
    assert_eq!(after.group_commands - before.group_commands, 2); // the red, then the level
    assert_eq!(after.unicasts - before.unicasts, 0);
    for light in lights() {
        assert_eq!(r.bulb(light).color, Some(RED));
    }

    r.world.run_for(secs(60)); // a beat a second
    assert_eq!(r.world.network.borrow().counts().refused_broadcasts, 0); // the queue keeps inside the stack's limit
}

#[test]
fn a_bulb_out_of_reach_goes_silent_then_catches_up_when_its_back() {
    let mut r = Room::new(Vec::new());
    r.world.run_for(secs(5));
    r.world.network.borrow_mut().set_reachable(ext_of(WALL_C), false);
    r.press(Press::Double);
    r.world.run_for(secs(200));
    assert_eq!(r.link(WALL_C), DeviceLink::Silent);
    assert!(r.bulb(WALL_C).color != Some(RED) && r.bulb(WALL_A).color == Some(RED));

    r.world.network.borrow_mut().set_reachable(ext_of(WALL_C), true);
    r.world.run_for(secs(30)); // its next ping answers
    assert_eq!(r.link(WALL_C), DeviceLink::Online);
    assert_eq!(r.bulb(WALL_C).color, Some(RED));
}

#[test]
fn a_button_that_isnt_bound_isnt_heard() {
    let mut world = World::new(noon(), room_addresses(), Vec::new());
    let status = Rc::new(RefCell::new(None::<Arc<StatusSnapshot>>));
    let latest = status.clone();
    world.bus.on::<StatusChanged>(move |e| *latest.borrow_mut() = Some(e.snapshot.clone()));
    world.network.borrow_mut().add(Device::new(ext_of(BUTTON), Kind::Button));
    world.network.borrow_mut().press(ext_of(BUTTON), Press::Double); // before it has joined, let alone been bound
    world.start();
    world.run_for(secs(5));
    assert_eq!(status.borrow().as_ref().unwrap().zones[ZoneId(0)].mode, Mode::Daylight);
}
