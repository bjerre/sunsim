//! The paced send queue: bulb commands out one at a time, grouped where they can be.

use std::collections::VecDeque;

use ev::*;

/// A Zigbee group: lights the bulbs themselves know as its members, so one
/// command to its id reaches them all at once.
#[derive(Clone, Copy, Debug)]
pub struct LightGroup {
    pub id: u16,
    pub members: RoleSet,
}

// A light's commands, in the order they go out.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Step {
    On,
    HueSat,
    Level,
    Color,
    Off,
}

fn first_step(s: &SetLight) -> Option<Step> {
    match s {
        SetLight { power: Some(true), .. } => Some(Step::On),
        SetLight { color: Some(Color::HueSat(_)), .. } => Some(Step::HueSat),
        SetLight { level: Some(_), .. } => Some(Step::Level),
        SetLight { color: Some(_), .. } => Some(Step::Color),
        SetLight { power: Some(false), .. } => Some(Step::Off),
        _ => None,
    }
}

// The command for `step`, to nowhere yet.
fn command_of(s: &SetLight, step: Step) -> SendLight {
    let mut c = SendLight::default();
    match step {
        Step::On | Step::Off => c.power = s.power,
        Step::Level => {
            c.level = s.level;
            c.transition = s.level_transition;
        }
        Step::HueSat | Step::Color => {
            c.color = s.color;
            c.transition = s.color_transition;
        }
    }
    c
}

fn sent(s: &mut SetLight, step: Step) {
    match step {
        Step::On | Step::Off => s.power = None,
        Step::Level => s.level = None,
        Step::HueSat | Step::Color => s.color = None,
    }
}

fn same_command(a: &SendLight, b: &SendLight) -> bool {
    a.power == b.power && a.level == b.level && a.color == b.color && a.transition == b.transition
}

/// Between LightOutput and the radio: holds each light's commands (SetLight)
/// until the ZigbeeAdapter has room for one (a SendSlot, a few ms apart while
/// any are waiting: SendsWaiting asks for the next), so a burst for every
/// light goes out paced instead of all at once. A newer SetLight for a light
/// merges over the one waiting. Each slot sends one ZCL command (SendLight):
/// to the largest group whose lights all wait for exactly that command (the
/// same step, value and transition), else to the light alone. The groups
/// fit whatever the lights are showing: red alert's whole room, an alert's
/// pulse on two wall lights while the others follow the sun. A light's
/// commands keep SetLight's order (On, a saturated color, the level, a
/// white or xy color, Off), and the lights take turns, the longest waiting
/// first. A group command is a broadcast, which the network allows few of:
/// past MAX_GROUP_SENDS in GROUP_SEND_WINDOW, they go to each light instead.
pub struct SendQueue {
    post: Post,
    groups: Vec<LightGroup>,
    waiting: PerRole<Option<SetLight>>,
    turns: Vec<Role>,               // the lights waiting, the next to send first
    group_sends: VecDeque<Instant>, // within the window, oldest first
    asked: bool,                    // SendsWaiting posted, its SendSlot not yet come
}

impl SendQueue {
    pub const MAX_GROUP_SENDS: usize = 8;
    pub const GROUP_SEND_WINDOW: Duration = Duration::secs(9);

    pub fn new(post: Post, groups: Vec<LightGroup>) -> Self {
        SendQueue {
            post,
            groups,
            waiting: PerRole::default(),
            turns: Vec::new(),
            group_sends: VecDeque::new(),
            asked: false,
        }
    }

    pub fn set_light(&mut self, e: &SetLight) {
        if !is_light(e.role) || first_step(e).is_none() {
            return;
        }
        match &mut self.waiting[e.role] {
            slot @ None => {
                self.turns.push(e.role);
                *slot = Some(*e);
            }
            Some(w) if e.power == Some(false) => *w = *e, // off: what was waiting doesn't matter any more
            Some(w) => {
                w.short_addr = e.short_addr;
                w.endpoint = e.endpoint;
                if e.power.is_some() {
                    w.power = e.power;
                }
                if e.level.is_some() {
                    w.level = e.level;
                    w.level_transition = e.level_transition;
                }
                if e.color.is_some() {
                    w.color = e.color;
                    w.color_transition = e.color_transition;
                }
            }
        }
        self.ask();
    }

    pub fn send_slot(&mut self, e: &SendSlot) {
        self.asked = false;
        let Some(&first) = self.turns.first() else { return };
        while self.group_sends.front().is_some_and(|&t| e.at - t >= Self::GROUP_SEND_WINDOW) {
            self.group_sends.pop_front();
        }

        let head = self.waiting[first].expect("a light takes turns while it has commands waiting");
        let step = first_step(&head).expect("waiting commands have a step");
        let mut send = command_of(&head, step);
        send.backlog = self.turns.len().min(255) as u8;

        // Every light waiting for exactly this command.
        let same: RoleSet = self
            .turns
            .iter()
            .copied()
            .filter(|&r| {
                let w = self.waiting[r].as_ref().unwrap();
                first_step(w) == Some(step) && same_command(&command_of(w, step), &send)
            })
            .collect();
        let group =
            if self.group_sends.len() < Self::MAX_GROUP_SENDS { self.largest_within(same, first) } else { None };
        let to = match group {
            Some(group) => {
                send.to = Destination { address: group.id, endpoint: 0, group: true };
                self.group_sends.push_back(e.at);
                group.members
            }
            None => {
                send.to = Destination { address: head.short_addr, endpoint: head.endpoint, group: false };
                RoleSet::EMPTY.with(first)
            }
        };
        self.post.send(send);

        // Each light it went to: on to its next command, at the back of the line.
        let (went, mut later): (Vec<Role>, Vec<Role>) = self.turns.iter().partition(|&&r| to.contains(r));
        for r in went {
            let w = self.waiting[r].as_mut().unwrap();
            sent(w, step);
            if first_step(w).is_some() {
                later.push(r);
            } else {
                self.waiting[r] = None;
            }
        }
        self.turns = later;
        self.ask();
    }

    pub fn reachability_changed(&mut self, e: &ReachabilityChanged) {
        if !e.reachable {
            self.drop_light(e.role); // LightOutput sends it everything when it's back
        }
    }

    // The largest group with `first` whose lights are all in `lights`: none
    // left out, so none gets a command that isn't its own.
    fn largest_within(&self, lights: RoleSet, first: Role) -> Option<LightGroup> {
        let mut best: Option<LightGroup> = None;
        for g in &self.groups {
            if g.members.len() >= 2
                && g.members.contains(first)
                && g.members.is_subset(lights)
                && best.is_none_or(|b| g.members.len() > b.members.len())
            {
                best = Some(*g);
            }
        }
        best
    }

    fn drop_light(&mut self, role: Role) {
        if is_light(role) {
            self.waiting[role] = None;
            self.turns.retain(|&r| r != role);
        }
    }

    fn ask(&mut self) {
        if !self.asked && !self.turns.is_empty() {
            self.asked = true;
            self.post.send(SendsWaiting);
        }
    }
}

impl Module for SendQueue {
    fn on(&mut self, e: &Event) {
        match e {
            Event::SetLight(e) => self.set_light(e),
            Event::SendSlot(e) => self.send_slot(e),
            Event::RoleCleared(e) => self.drop_light(e.role),
            Event::ReachabilityChanged(e) => self.reachability_changed(e),
            _ => {}
        }
    }
}
