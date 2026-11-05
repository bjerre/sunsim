//! Which device holds which role, and whether it's alive.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use ev::*;

const TAG: &str = "REGISTRY";

const DISCOVERY_RETRY: Duration = Duration::secs(15);
const FIRST_PING: Duration = Duration::secs(15); // after the network is up, or pairing
const PING_INTERVAL: Duration = Duration::secs(60); // reachable lights
const UNREACHABLE_PING: Duration = Duration::secs(15); // until they answer again
const UNREACHABLE_AFTER: Duration = Duration::secs(60); // of silence, plus a failed ping
const ONLINE_WINDOW: Duration = Duration::secs(150); // shown as online: a couple of missed pings

// The lights take turns: each is first pinged at its own point in the
// interval, by slot, so the pings spread out instead of all going at once.
// In whole seconds.
fn ping_turn(light: Role) -> Duration {
    Duration::secs(PING_INTERVAL.floor_secs() * slot(light) as i64 / ROLE_COUNT as i64)
}

#[derive(Clone, Copy, Debug)]
struct Liveness {
    last_heard: Option<Instant>, // none: not since boot
    rssi: i8,
    reachable: bool,
}

impl Default for Liveness {
    fn default() -> Self {
        Liveness { last_heard: None, rssi: 0, reachable: true }
    }
}

#[derive(Default)]
struct State {
    records: DeviceRecords,
    liveness: PerRole<Liveness>,
    armed: Option<Role>,
    network_up: bool, // nothing is sent before
}

// What the flows share with the handlers.
struct Shared {
    post: Post,
    addresses: Addresses,
    timer: Timer,
    network: Inbox<NetworkUp>,
    endpoints: Inbox<EndpointFound>,
    pings: Inbox<PingResult>,
    state: RefCell<State>,
}

/// Which device holds which role, and whether it's alive.
///  - Pairing: a device announcing itself while a role is armed (console
///    `pair`) gets that role, unless it already holds one; otherwise its
///    address in the installation (ADDRESSES) decides; anything else is
///    ignored.
///  - Discovery (a flow per role): finds the device's endpoint
///    (Discover/EndpointFound), retried every 15 s until it's known; the
///    button is then bound (Bind).
///  - Liveness (a flow per light): acks, ping replies, announces and button
///    presses count as "heard". Lights are pinged 15 s after the network is
///    up (or after pairing), plus their turn (spread across the minute by
///    slot), then every 60 s. A failed or unanswered ping with nothing heard
///    for a minute marks a light unreachable (ReachabilityChanged): its
///    commands pause - sending to a missing device ties up the stack's
///    buffers with retries and starves everything else - and it's pinged
///    every 15 s until it answers anything, when it's marked reachable again
///    and its light re-applied. Heard within 150 s (and reachable) shows as
///    Online in DevicesUpdated, otherwise Silent.
///
/// Persists through PersistRoles; never touches storage or the radio itself.
pub struct DeviceRegistry {
    s: Rc<Shared>,
    // Flows; reassigning a slot restarts it, Flow::default() stops it.
    discovery: PerRole<Flow>,
    keep_alive: PerRole<Flow>, // lights only
}

impl Shared {
    fn record(&self, role: Role) -> DeviceRecord {
        self.state.borrow().records[role]
    }

    fn role_by_ext(&self, ext_addr: u64) -> Option<Role> {
        let st = self.state.borrow();
        ROLES.into_iter().find(|&r| st.records[r].valid && st.records[r].ext_addr == ext_addr)
    }

    fn role_from_address(&self, ext_addr: u64) -> Option<Role> {
        ROLES.into_iter().find(|&r| ext_addr != 0 && self.addresses[r] == ext_addr)
    }

    fn persist(&self) {
        let records = self.state.borrow().records;
        self.post.send(PersistRoles { records: Arc::new(records) });
    }

    fn heard(&self, role: Role) {
        self.state.borrow_mut().liveness[role].last_heard = Some(self.timer.now());
        self.set_reachable(role, true);
    }

    fn set_reachable(&self, role: Role, reachable: bool) {
        {
            let mut st = self.state.borrow_mut();
            let l = &mut st.liveness[role];
            if l.reachable == reachable {
                return;
            }
            l.reachable = reachable;
        }
        self.post.send(ReachabilityChanged { role, reachable });
        if reachable {
            log::info!(target: TAG, "{} is answering again - resuming its light", role_name(role));
            if is_light(role) {
                self.post.send(RefreshLights { role: Some(role) });
            }
        } else {
            log::warn!(
                target: TAG,
                "{} not answering - pausing its light commands (powered off or out of range?)",
                role_name(role)
            );
        }
    }

    fn link(&self, role: Role) -> DeviceLink {
        let st = self.state.borrow();
        let l = st.liveness[role];
        if !st.records[role].valid {
            return DeviceLink::Unpaired;
        }
        if l.reachable && l.last_heard.is_some_and(|h| self.timer.now() - h <= ONLINE_WINDOW) {
            return DeviceLink::Online;
        }
        DeviceLink::Silent
    }

    fn endpoint_found(&self, role: Role, endpoint: u8) {
        let d = {
            let mut st = self.state.borrow_mut();
            st.records[role].endpoint = endpoint;
            st.records[role]
        };
        self.persist();
        self.post.send(RoleAssigned { role, record: d });
        if is_button(role) {
            log::info!(target: TAG, "{} endpoint = {}, binding to local receiver", role_name(role), d.endpoint);
            self.post.send(Bind { role, ext_addr: d.ext_addr, short_addr: d.short_addr, endpoint: d.endpoint });
        } else {
            log::info!(target: TAG, "{} (light) endpoint = {}", role_name(role), d.endpoint);
            self.post.send(RefreshLights { role: Some(role) });
        }
    }

    fn publish_devices(&self) {
        let mut devices = PerRole::<DeviceStatus>::default();
        for role in ROLES {
            let link = self.link(role);
            let st = self.state.borrow();
            let l = st.liveness[role];
            devices[role] = DeviceStatus {
                link,
                paired: st.records[role].valid,
                has_endpoint: st.records[role].endpoint != 0,
                last_heard: l.last_heard,
                rssi: l.rssi,
                reachable: l.reachable,
            };
        }
        let st = self.state.borrow();
        self.post.send(DevicesUpdated { devices: Arc::new(devices), records: Arc::new(st.records), armed: st.armed });
    }
}

async fn find_endpoint(s: Rc<Shared>, role: Role) {
    if !s.state.borrow().network_up {
        s.network.next().await; // roles restored at boot: ask once there's a network
    }
    loop {
        let retry_at = s.timer.now() + DISCOVERY_RETRY;
        s.post.send(Discover { role, short_addr: s.record(role).short_addr });
        // A reply without one (the adapter logged why) waits out the interval.
        while let Some(found) = s.endpoints.next_where_by(move |e| e.role == role, retry_at).await {
            if found.endpoint != 0 {
                return s.endpoint_found(role, found.endpoint);
            }
        }
        log::info!(
            target: TAG,
            "Retrying endpoint discovery for {} (short=0x{:04x})",
            role_name(role),
            s.record(role).short_addr
        );
    }
}

// A button is a sleepy end device that won't answer pings; it counts as
// heard when it's pressed instead.
async fn keep_alive(s: Rc<Shared>, light: Role) {
    if !s.state.borrow().network_up {
        s.network.next().await;
    }
    s.timer.after(FIRST_PING + ping_turn(light)).await;
    loop {
        let sent = s.timer.now();
        s.post.send(Ping { role: light, short_addr: s.record(light).short_addr });
        let reply = s.pings.next_where_by(move |p| p.role == light, sent + UNREACHABLE_PING).await;
        if reply.is_some_and(|r| r.ok) {
            s.heard(light);
        } else {
            // A lost ping alone isn't enough (a busy network drops some); it
            // must also have been silent - no acks - for a while.
            let last_heard = s.state.borrow().liveness[light].last_heard;
            if last_heard.is_none_or(|h| s.timer.now() - h > UNREACHABLE_AFTER) {
                log::debug!(
                    target: TAG,
                    "Ping to {} failed (err=0x{:x} status=0x{:x})",
                    role_name(light),
                    reply.map_or(-1, |r| r.error),
                    reply.map_or(-1, |r| r.status)
                );
                s.set_reachable(light, false);
            }
        }
        let reachable = s.state.borrow().liveness[light].reachable;
        s.timer.until(sent + if reachable { PING_INTERVAL } else { UNREACHABLE_PING }).await;
    }
}

impl DeviceRegistry {
    /// `addresses`: who takes which role when it joins (ADDRESS_OF).
    pub fn new(post: Post, addresses: Addresses) -> Self {
        let wake = post.clone();
        let timer = Timer::with_wake(move |at| wake.send(WakeAt { at }));
        let s = Rc::new(Shared {
            post,
            addresses,
            network: Inbox::new(&timer),
            endpoints: Inbox::new(&timer),
            pings: Inbox::new(&timer),
            timer,
            state: RefCell::new(State::default()),
        });
        DeviceRegistry { s, discovery: PerRole::default(), keep_alive: PerRole::default() }
    }

    fn discover(&mut self, role: Role) {
        self.discovery[role] = Flow::start(find_endpoint(self.s.clone(), role));
    }

    fn keep_alive(&mut self, light: Role) {
        self.keep_alive[light] = Flow::start(keep_alive(self.s.clone(), light));
    }

    fn assign(&mut self, role: Role, ext_addr: u64, short_addr: u16) {
        {
            let mut st = self.s.state.borrow_mut();
            st.records[role] = DeviceRecord { valid: true, ext_addr, short_addr, endpoint: 0 };
            let l = &mut st.liveness[role];
            *l = Liveness { reachable: l.reachable, ..Liveness::default() }; // heard() reports it reachable if it wasn't
        }
        self.s.persist();
        self.s.heard(role);
        self.s.post.send(RoleAssigned { role, record: self.s.record(role) });
        self.discover(role);
        if is_light(role) {
            self.keep_alive(role);
        }
    }

    fn clear(&mut self, role: Role) {
        self.discovery[role] = Flow::default();
        self.keep_alive[role] = Flow::default();
        {
            let mut st = self.s.state.borrow_mut();
            st.records[role] = DeviceRecord::default();
            st.liveness[role] = Liveness::default();
        }
        self.s.post.send(RoleCleared { role });
    }

    fn roles_loaded(&mut self, e: &RolesLoaded) {
        self.s.state.borrow_mut().records = *e.records;
        for role in ROLES {
            let record = self.s.record(role);
            if !record.valid {
                continue;
            }
            self.s.post.send(RoleAssigned { role, record });
            if record.endpoint == 0 {
                self.discover(role);
            }
            if is_light(role) {
                self.keep_alive(role);
            }
        }
    }

    fn device_announced(&mut self, e: &DeviceAnnounced) {
        let s = self.s.clone();
        if let Some(existing) = s.role_by_ext(e.ext_addr) {
            let moved = {
                let mut st = s.state.borrow_mut();
                let d = &mut st.records[existing];
                std::mem::replace(&mut d.short_addr, e.short_addr) != e.short_addr
            };
            if moved {
                s.persist();
            }
            log::info!(target: TAG, "Known device rejoined: {} short=0x{:04x}", role_name(existing), e.short_addr);
            s.heard(existing);
            let d = s.record(existing);
            s.post.send(RoleAssigned { role: existing, record: d });
            if is_button(existing) || d.endpoint == 0 {
                self.discover(existing); // a button is re-bound
            } else {
                s.post.send(RefreshLights { role: Some(existing) });
            }
            return;
        }

        let armed = s.state.borrow_mut().armed.take();
        if let Some(role) = armed {
            log::info!(
                target: TAG,
                "Paired ext=0x{:016x} short=0x{:04x} -> {}",
                e.ext_addr,
                e.short_addr,
                role_name(role)
            );
            self.assign(role, e.ext_addr, e.short_addr);
            return;
        }

        let Some(role) = s.role_from_address(e.ext_addr) else {
            log::warn!(
                target: TAG,
                "Ignoring unknown device 0x{:04x} (ext=0x{:016x}): not in the installation's addresses (use `pair \
                 <role>` to claim it)",
                e.short_addr,
                e.ext_addr
            );
            return;
        };
        if s.record(role).valid {
            log::warn!(
                target: TAG,
                "Ignoring device 0x{:04x}: {} already assigned to a different device",
                e.short_addr,
                role_name(role)
            );
            return;
        }
        log::info!(target: TAG, "New device joined -> {} (by its address)", role_name(role));
        self.assign(role, e.ext_addr, e.short_addr);
    }

    fn ack_received(&mut self, e: &AckReceived) {
        for role in ROLES {
            let record = self.s.record(role);
            if record.valid && record.short_addr == e.short_addr {
                self.s.heard(role);
                self.s.state.borrow_mut().liveness[role].rssi = e.rssi;
            }
        }
    }

    fn pair_requested(&mut self, e: &PairRequested) {
        let name = role_name(e.role);
        if self.s.record(e.role).valid {
            log::warn!(target: TAG, "{name} is already assigned; the next device to join will replace it");
        }
        self.s.state.borrow_mut().armed = Some(e.role);
        self.s.post.send(OpenNetwork::default());
        log::info!(
            target: TAG,
            "Armed {name}: factory-reset / power on the device now. The next device that joins and doesn't already \
             hold a role becomes {name}."
        );
    }

    fn unpair_requested(&mut self, e: &UnpairRequested) {
        let all = e.roles == Selection::AllRoles;
        for role in ROLES.into_iter().filter(|&r| selects(e.roles, r)) {
            self.clear(role);
            if !all {
                log::info!(target: TAG, "{} cleared", role_name(role));
            }
        }
        if all {
            log::info!(target: TAG, "All roles cleared");
        }
        self.s.persist();
    }
}

impl Module for DeviceRegistry {
    fn on(&mut self, e: &Event) {
        match e {
            Event::RolesLoaded(e) => self.roles_loaded(e),
            Event::NetworkUp(e) => {
                self.s.state.borrow_mut().network_up = true;
                self.s.network.deliver(e, Some(e.at));
            }
            Event::DeviceAnnounced(e) => self.device_announced(e),
            Event::EndpointFound(e) => self.s.endpoints.deliver(e, None),
            Event::BindResult(e) => {
                if e.ok {
                    log::info!(target: TAG, "Bound {} to local receiver endpoint", role_name(e.role));
                } else {
                    log::error!(target: TAG, "Failed to bind {}", role_name(e.role));
                }
            }
            Event::PingResult(e) => self.s.pings.deliver(e, None),
            Event::AckReceived(e) => self.ack_received(e),
            Event::ButtonPressed(e) => {
                if e.source == Source::Zigbee && self.s.record(e.button).valid {
                    self.s.heard(e.button);
                }
            }
            Event::PairRequested(e) => self.pair_requested(e),
            Event::PairCanceled(_) => {
                self.s.state.borrow_mut().armed = None;
                log::info!(target: TAG, "Pairing canceled");
            }
            Event::UnpairRequested(e) => self.unpair_requested(e),
            Event::Tick(e) => {
                self.s.timer.tick(e.at);
                self.s.publish_devices();
            }
            Event::Wake(e) => self.s.timer.tick(e.at),
            _ => {}
        }
    }
}
