//! The room: which device is which, and the zones: what each light shows,
//! when it may be on, and what chooses its mode. The zones come from
//! config/installation.json (ev/build.rs), checked as they compile; a
//! runtime profile (profile.rs) can regroup and reschedule the same lights
//! instead. Either way the app runs on an Installation. The words are
//! topology.rs's.

use std::ops::{Index, IndexMut};

use crate::topology::*;
use crate::types::*;
#[allow(unused_imports)] // what the generated zones use depends on the document
use crate::{setting, time::Duration};

/// Each device's IEEE (EUI-64) address, from its label: when it joins, it
/// takes that role. A role left at 0 is claimed with `pair <role>` instead.
/// (`./sunsim monitor` logs an unknown device's address as it joins.) Kept
/// here, not in the shareable installation document.
pub const ADDRESSES: [Address; ROLE_COUNT] = [
    Address { role: "WallA", ext_addr: 0x0000000000000000 },
    Address { role: "WallB", ext_addr: 0x0000000000000000 },
    Address { role: "WallC", ext_addr: 0x0000000000000000 },
    Address { role: "WallD", ext_addr: 0x0000000000000000 },
    Address { role: "WallE", ext_addr: 0x0000000000000000 },
    Address { role: "Work", ext_addr: 0x0000000000000000 },
    Address { role: "Desk", ext_addr: 0x0000000000000000 },
    Address { role: "Grow", ext_addr: 0x0000000000000000 },
    Address { role: "Button", ext_addr: 0x0000000000000000 },
];

include!(concat!(env!("OUT_DIR"), "/zones.rs"));

// ---- Derived: the tables above per role, checked as they compile ----

pub const ADDRESS_OF: Addresses = addresses_of(&ADDRESSES);

/// The built-in installation's zones per role.
pub const TOPOLOGY: Topology = topology_of(&ZONES);
/// Each light's default peak brightness, as the built-in zones give it.
pub const DEFAULT_PEAKS: Peaks = TOPOLOGY.peak_of;

/// The most zones an installation has: what a PerZone holds.
pub const MAX_ZONES: usize = 8;
const _: () = assert!(!ZONES.is_empty() && ZONES.len() <= MAX_ZONES, "1-8 zones");

// ---- At run time ----------------------------------------------------------

/// A zone, as the app runs it: its name, gate, modes and buttons. (Its
/// lights, their looks and weather are the topology's, per role.)
#[derive(Clone, Debug)]
pub struct Zone {
    pub name: String,
    pub gate: Vec<Rule>,
    pub modes: Vec<Mode>,
    pub controls: Vec<Controls>,
}

impl Zone {
    pub fn allows(&self, mode: Mode) -> bool {
        self.modes.contains(&mode)
    }
}

/// The zones the app runs, and what they make of each role: the built-in
/// ones (ZONES) or a profile's.
#[derive(Clone, Debug)]
pub struct Installation {
    pub zones: Vec<Zone>,
    pub topology: Topology,
}

impl Default for Installation {
    fn default() -> Self {
        Installation::built_in()
    }
}

impl Installation {
    /// The zones the build was made with.
    pub fn built_in() -> Installation {
        let zones = ZONES
            .iter()
            .map(|z| Zone {
                name: z.name.to_string(),
                gate: z.gate.to_vec(),
                modes: z.modes.to_vec(),
                controls: z.controls.to_vec(),
            })
            .collect();
        Installation { zones, topology: TOPOLOGY }
    }

    /// Every zone, in order.
    pub fn zone_ids(&self) -> impl Iterator<Item = ZoneId> + use<> {
        (0..self.zones.len() as u8).map(ZoneId)
    }

    pub fn spec(&self, zone: ZoneId) -> &Zone {
        &self.zones[zone.0 as usize]
    }
}

/// One T per zone, indexed by the zone (room for MAX_ZONES).
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct PerZone<T>(pub [T; MAX_ZONES]);

impl<T: Default> Default for PerZone<T> {
    fn default() -> Self {
        PerZone(std::array::from_fn(|_| T::default()))
    }
}

impl<T> Index<ZoneId> for PerZone<T> {
    type Output = T;
    fn index(&self, zone: ZoneId) -> &T {
        &self.0[zone.0 as usize]
    }
}
impl<T> IndexMut<ZoneId> for PerZone<T> {
    fn index_mut(&mut self, zone: ZoneId) -> &mut T {
        &mut self.0[zone.0 as usize]
    }
}
