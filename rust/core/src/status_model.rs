//! One snapshot of everything the status shows.

use std::sync::Arc;

use ev::*;

use crate::gate::openness;
use crate::math::day_info::compute_day_info;

/// Gathers what the status display and console show into one snapshot,
/// published as StatusChanged once a second (after each Tick's DevicesUpdated).
pub struct StatusModel {
    post: Post,
    snap: StatusSnapshot,
}

impl StatusModel {
    pub fn new(post: Post) -> Self {
        StatusModel { post, snap: StatusSnapshot::default() }
    }

    fn publish(&mut self) {
        let t = self.snap.time;
        self.snap.day = compute_day_info(&t, self.snap.config.latitude_deg, self.snap.config.longitude_deg);
        for zone in zone_ids() {
            self.snap.zones[zone].gate_open = openness(spec_of(zone).gate, &t, &self.snap.config) > 0.0;
        }
        self.post.send(StatusChanged { snapshot: Arc::new(self.snap.clone()) });
    }
}

impl Module for StatusModel {
    fn on(&mut self, e: &Event) {
        match e {
            Event::ConfigChanged(e) => {
                self.snap.config = e.config;
                self.snap.peaks = e.peaks;
                self.snap.min_levels = e.min_levels;
            }
            Event::ModeChanged(e) => self.snap.zones[e.zone].mode = e.mode,
            Event::DevicesUpdated(e) => {
                self.snap.devices = *e.devices;
                self.snap.records = *e.records;
                self.snap.armed = e.armed;
                self.publish();
            }
            Event::Tick(e) => {
                // Published once the registry's DevicesUpdated for this tick
                // arrives (it posts one every Tick), so device links reflect
                // this second, not the last.
                self.snap.at = e.at;
                self.snap.time = e.time;
            }
            Event::SkyChanged(e) => self.snap.sky = e.sky,
            _ => {}
        }
    }
}
