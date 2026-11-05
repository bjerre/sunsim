//! A simulated Zigbee network.

use std::collections::VecDeque;

use ev::*;

use crate::scheduler::Scheduler;

/// A device in the simulated network, as it's set up: what it is, and how
/// long a frame takes to reach it.
#[derive(Clone, Copy, Debug)]
pub struct Device {
    pub ext_addr: u64,
    pub kind: Kind,
    pub endpoint: u8,
    pub latency: Duration, // each way
    pub rssi: i8,
}

impl Device {
    /// A device of `kind` at `ext_addr`, nearby.
    pub fn new(ext_addr: u64, kind: Kind) -> Self {
        Device { ext_addr, kind, endpoint: 11, latency: Duration::millis(20), rssi: -55 }
    }
}

/// A light as its ZCL clusters hold it. The level moves to its target over
/// the command's transition, as a bulb's does; the color is the last one
/// sent (when it's shown is all the tests need).
#[derive(Clone, Copy, Default, Debug)]
pub struct Bulb {
    pub on: bool,
    pub color: Option<Color>,
    pub from: u8, // the level moving from `from` to `to`
    pub to: u8,
    pub since: Instant,
    pub over: Duration,
}

impl Bulb {
    /// What it shows at `at`: 0 when it's off.
    pub fn level_at(&self, at: Instant) -> u8 {
        if !self.on {
            return 0;
        }
        if self.over <= Duration::ZERO || at >= self.since + self.over {
            return self.to;
        }
        let done = if at <= self.since { 0.0 } else { (at - self.since).as_secs_f64() / self.over.as_secs_f64() };
        (self.from as f64 + (self.to as f64 - self.from as f64) * done).round() as u8
    }
}

/// What the network has carried, for tests.
#[derive(Clone, Copy, Default, Debug)]
pub struct Counts {
    pub unicasts: u32,
    pub group_commands: u32,
    pub refused_broadcasts: u32, // no room in the broadcast table
    pub lost: u32,               // sent to a device out of reach
}

#[derive(Clone, Debug)]
struct Node {
    device: Device,
    short_addr: u16,
    joined: bool,
    reachable: bool,
    bound: bool, // a button's On/Off output, to us
    bulb: Bulb,
    groups: Vec<u16>,
}

// The button, as the ZigbeeAdapter names its presses.
const BUTTON: Role = {
    let mut i = 0;
    while i < ROLE_COUNT && !is_button(ROLES[i]) {
        i += 1;
    }
    if i < ROLE_COUNT { ROLES[i] } else { ROLES[0] }
};

const FIRST_SHORT: u16 = 0x1001;

/// The board's radio, simulated: the ZigbeeAdapter's events, answered as the
/// network would. Devices join while the network is open (pairing, or the
/// few minutes after it's up), each bulb holds what it's sent, and a device
/// out of reach answers nothing. Like the real stack, it holds a broadcast
/// (a group command) for BROADCAST_HELD and has room for BROADCAST_SLOTS at
/// a time: past that, a group command is refused. Group membership is set by
/// hand (join_group()) until sunsim adds bulbs to groups itself.
pub struct SimNetwork {
    scheduler: Scheduler,
    nodes: Vec<Node>,
    open_until: Instant,
    broadcasts: VecDeque<Instant>, // held, oldest first
    slot_due: bool,
    counts: Counts,
}

impl SimNetwork {
    pub const BROADCAST_SLOTS: usize = 9;
    pub const BROADCAST_HELD: Duration = Duration::secs(9);
    pub const SEND_GAP: Duration = Duration::millis(25); // as the ZigbeeAdapter paces commands
    pub const NO_ANSWER: Duration = Duration::secs(3); // a request to a device out of reach: given up on
    pub const FORMING: Duration = Duration::secs(2); // from start() to the network being up

    pub fn new(scheduler: Scheduler) -> Self {
        SimNetwork {
            scheduler,
            nodes: Vec::new(),
            open_until: Instant::ZERO,
            broadcasts: VecDeque::new(),
            slot_due: false,
            counts: Counts::default(),
        }
    }

    /// The network forms: NetworkUp FORMING later, and it's open for a while.
    pub fn start(&mut self) {
        let up = self.scheduler.now() + Self::FORMING;
        self.scheduler.at(up, NetworkUp { rejoined: false, at: up });
    }

    /// A device is powered on: it joins as soon as the network is open.
    /// Returns its short address.
    pub fn add(&mut self, device: Device) -> u16 {
        let short_addr = FIRST_SHORT + self.nodes.len() as u16;
        self.nodes.push(Node {
            device,
            short_addr,
            joined: false,
            reachable: true,
            bound: false,
            bulb: Bulb::default(),
            groups: Vec::new(),
        });
        let i = self.nodes.len() - 1;
        self.join_if_open(i);
        short_addr
    }

    pub fn set_reachable(&mut self, ext_addr: u64, reachable: bool) {
        if let Some(node) = self.by_ext(ext_addr) {
            node.reachable = reachable;
        }
    }

    pub fn join_group(&mut self, ext_addr: u64, group: u16) {
        if let Some(node) = self.by_ext(ext_addr)
            && !node.groups.contains(&group)
        {
            node.groups.push(group);
        }
    }

    /// A press on a button, which arrives if the button is bound to us.
    pub fn press(&mut self, button: u64, press: Press) {
        let now = self.scheduler.now();
        if let Some(node) = self.by_ext(button).filter(|n| n.joined && n.reachable && n.bound) {
            let at = now + node.device.latency;
            self.scheduler.at(at, ButtonPressed { button: BUTTON, press, source: ev::Source::Zigbee, at });
        }
    }

    pub fn bulb(&self, ext_addr: u64) -> Option<&Bulb> {
        self.nodes.iter().find(|n| n.device.ext_addr == ext_addr).map(|n| &n.bulb)
    }

    pub fn counts(&self) -> Counts {
        self.counts
    }

    fn by_short(&mut self, short_addr: u16) -> Option<&mut Node> {
        self.nodes.iter_mut().find(|n| n.short_addr == short_addr && n.joined)
    }

    fn by_ext(&mut self, ext_addr: u64) -> Option<&mut Node> {
        self.nodes.iter_mut().find(|n| n.device.ext_addr == ext_addr)
    }

    fn join_if_open(&mut self, i: usize) {
        let now = self.scheduler.now();
        let node = &mut self.nodes[i];
        if node.joined || !node.reachable || now >= self.open_until {
            return;
        }
        node.joined = true;
        self.scheduler.after(
            node.device.latency,
            DeviceAnnounced { ext_addr: node.device.ext_addr, short_addr: node.short_addr },
        );
    }

    fn apply(node: &mut Node, e: &SendLight, now: Instant) {
        let b = &mut node.bulb;
        let at = now + node.device.latency;
        if let Some(power) = e.power {
            b.on = power;
        } else if let Some(level) = e.level {
            b.from = b.level_at(at);
            b.to = level;
            b.since = at;
            b.over = e.transition.duration();
            b.on = level > 0; // Move to Level with On/Off
        } else if e.color.is_some() {
            b.color = e.color;
        }
    }

    // A request's answer: after the round trip, or NO_ANSWER's `missing` if
    // the device at `short_addr` is out of reach (nothing, if that's None).
    fn answer(&mut self, short_addr: u16, reply: impl Into<Event>, missing: Option<Event>) {
        let latency = self.by_short(short_addr).filter(|n| n.reachable).map(|n| n.device.latency);
        match (latency, missing) {
            (Some(latency), _) => self.scheduler.after(latency * 2, reply),
            (None, Some(missing)) => self.scheduler.after(Self::NO_ANSWER, missing),
            (None, None) => {}
        }
    }
}

impl Radio for SimNetwork {
    fn sends_waiting(&mut self, _: &SendsWaiting) {
        if !self.slot_due {
            self.slot_due = true;
            self.scheduler.after(Self::SEND_GAP, SendSlot { at: self.scheduler.now() + Self::SEND_GAP });
        }
    }

    fn send_light(&mut self, e: &SendLight) {
        let now = self.scheduler.now();
        if !e.to.group {
            self.counts.unicasts += 1;
            let Some(node) = self.by_short(e.to.address).filter(|n| n.reachable) else {
                self.counts.lost += 1;
                return;
            };
            Self::apply(node, e, now);
            let ack = AckReceived { short_addr: node.short_addr, rssi: node.device.rssi, status: 0 };
            let latency = node.device.latency;
            self.scheduler.after(latency * 2, ack);
            return;
        }
        while self.broadcasts.front().is_some_and(|&t| now - t >= Self::BROADCAST_HELD) {
            self.broadcasts.pop_front();
        }
        if self.broadcasts.len() >= Self::BROADCAST_SLOTS {
            self.counts.refused_broadcasts += 1;
            return;
        }
        self.broadcasts.push_back(now);
        self.counts.group_commands += 1;
        for node in self.nodes.iter_mut().filter(|n| n.joined && n.reachable && n.groups.contains(&e.to.address)) {
            Self::apply(node, e, now);
        }
    }

    fn discover(&mut self, e: &Discover) {
        let endpoint =
            self.by_short(e.short_addr).filter(|n| n.device.kind == kind_of(e.role)).map_or(0, |n| n.device.endpoint);
        self.answer(
            e.short_addr,
            EndpointFound { role: e.role, endpoint },
            Some(EndpointFound { role: e.role, endpoint: 0 }.into()),
        );
    }

    fn bind(&mut self, e: &Bind) {
        if let Some(node) = self.by_short(e.short_addr).filter(|n| n.reachable) {
            node.bound = true;
        }
        self.answer(
            e.short_addr,
            BindResult { role: e.role, ok: true },
            Some(BindResult { role: e.role, ok: false }.into()),
        );
    }

    fn ping(&mut self, e: &Ping) {
        self.answer(
            e.short_addr,
            PingResult { role: e.role, ok: true, ..PingResult::default() },
            Some(PingResult { role: e.role, ok: false, error: 1, status: -1 }.into()),
        );
    }

    fn open_network(&mut self, e: &OpenNetwork) {
        self.open_until = self.scheduler.now() + Duration::secs(e.secs as i64);
        for i in 0..self.nodes.len() {
            self.join_if_open(i);
        }
    }

    fn factory_reset(&mut self, _: &FactoryReset) {
        // The board would restart with an empty network: every device leaves,
        // and joins again when it's open.
        for node in &mut self.nodes {
            node.joined = false;
            node.bound = false;
            node.groups.clear();
        }
        self.broadcasts.clear();
        self.start();
    }
}

impl Module for SimNetwork {
    fn on(&mut self, e: &Event) {
        if requests(self, e) {
            return;
        }
        match e {
            // Up: open, as the ZigbeeAdapter opens it then, for the devices waiting to join.
            Event::NetworkUp(_) => self.open_network(&OpenNetwork::default()),
            Event::SendSlot(_) => self.slot_due = false,
            _ => {}
        }
    }
}
