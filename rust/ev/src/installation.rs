//! The room, as data: which device is which, and the zones: what each light
//! shows, when it may be on, and what chooses its mode. Edit, build, flash.
//! The words are topology.rs's.

use std::ops::{Index, IndexMut};

use crate::setting;
use crate::topology::*;
use crate::types::*;

/// Each device's IEEE (EUI-64) address, from its label: when it joins, it
/// takes that role. A role left at 0 is claimed with `pair <role>` instead.
/// (`./sunsim monitor` logs an unknown device's address as it joins.)
pub const ADDRESSES: [Address; ROLE_COUNT] = [
    Address { role: "WallA", ext_addr: 0x0000000000000000 },
    Address { role: "WallB", ext_addr: 0x0000000000000000 },
    Address { role: "WallC", ext_addr: 0x0000000000000000 },
    Address { role: "WallD", ext_addr: 0x0000000000000000 },
    Address { role: "WallE", ext_addr: 0x0000000000000000 },
    Address { role: "Work", ext_addr: 0x0000000000000000 },
    Address { role: "Button", ext_addr: 0x0000000000000000 },
];

/// Lights that share a mode and a gate. Every light is in exactly one.
pub const ZONES: [ZoneSpec; 1] = [ZoneSpec {
    name: "office",
    lights: &[
        Shows { lights: "Wall", look: Look::SunCurve },
        Shows { lights: "Work", look: Look::WorkCurve(WorkCurve::SETTINGS) },
    ],
    // The active hours (`config hours`, `days`, `fade`).
    gate: &[Window { hours: setting!(active), days: setting!(active_days), fade: setting!(fade) }.rule()],
    controls: &[Controls::by("Button")], // single: daylight, double: red alert, hold: off
    weather: Some(Weather { precip: &["WallC"], alerts: &["WallB", "WallD"] }),
    ..ZoneSpec::DEFAULT
}];

// ---- Derived: the tables above per role and per zone, checked as they compile ----

pub const ADDRESS_OF: Addresses = addresses_of(&ADDRESSES);

pub const TOPOLOGY: Topology = topology_of(&ZONES);
pub const ZONE_OF: PerRole<Option<ZoneId>> = TOPOLOGY.zone_of;
pub const LOOK_OF: PerRole<Look> = TOPOLOGY.look_of;
pub const CONTROLS_OF: PerRole<Option<Controls>> = TOPOLOGY.controls_of;
pub const WEATHER_LIGHTS: WeatherLights = TOPOLOGY.weather;

pub const ZONE_COUNT: usize = ZONES.len();
const _: () = assert!(ZONE_COUNT >= 1 && ZONE_COUNT <= 255, "1-255 zones");

pub fn spec_of(zone: ZoneId) -> &'static ZoneSpec {
    &ZONES[zone.0 as usize]
}

/// Every zone, in order.
pub fn zone_ids() -> impl Iterator<Item = ZoneId> {
    (0..ZONE_COUNT as u8).map(ZoneId)
}

/// One T per zone, indexed by the zone.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct PerZone<T>(pub [T; ZONE_COUNT]);

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
