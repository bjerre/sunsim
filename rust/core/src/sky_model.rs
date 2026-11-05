//! What the sky is now.

use ev::*;

use crate::math::sky::cloud_cover;

/// What the sky is now, from the forecast (daily) and the live conditions
/// and alerts (every 15 minutes), which count only while fresh
/// (WEATHER_FRESH_FOR) and, for alerts, until they end. Re-judged on every
/// Tick and each update; posts SkyChanged whenever the conclusion changes,
/// so nobody else keeps the weather or a clock for it.
///
/// The cloud cover is the live reading while it's fresh, else the
/// forecast's for the time; fast-day's simulated day always reads the
/// forecast.
pub struct SkyModel {
    post: Post,
    forecast: CloudCover,
    conditions: Option<ConditionsUpdated>,
    alerts: Option<AlertsUpdated>,
    now: Instant,      // the latest Tick's
    time: TimeReading, // likewise
    posted: Option<Sky>,
}

impl SkyModel {
    pub fn new(post: Post) -> Self {
        SkyModel {
            post,
            forecast: CloudCover::default(),
            conditions: None,
            alerts: None,
            now: Instant::ZERO,
            time: TimeReading::default(),
            posted: None,
        }
    }

    fn fresh(&self, at: Instant) -> bool {
        self.now - at < WEATHER_FRESH_FOR
    }

    fn update(&mut self) {
        let sky = self.judge();
        if self.posted != Some(sky) {
            self.posted = Some(sky);
            self.post.send(SkyChanged { sky });
        }
    }

    fn judge(&self) -> Sky {
        let mut sky = Sky::default();
        if let Some(c) = self.conditions.filter(|c| self.fresh(c.at)) {
            sky.conditions = Some(c.now);
        }
        match sky.conditions {
            Some(now) if !self.time.simulated => {
                sky.cloud_percent = Some(now.cloud_percent);
                sky.cloud_live = true;
            }
            _ => sky.cloud_percent = cloud_cover(&self.forecast, &self.time).map(|c| (c * 100.0).round() as u8),
        }
        if let Some(a) = self.alerts.filter(|a| self.fresh(a.at)) {
            for alert in a.alerts.list().iter().filter(|alert| alert.until > self.now) {
                sky.alerts.push(*alert);
            }
        }
        sky
    }
}

impl Module for SkyModel {
    fn on(&mut self, e: &Event) {
        match e {
            Event::Tick(e) => {
                self.now = e.at;
                self.time = e.time;
            }
            Event::CloudCoverUpdated(e) => self.forecast = e.forecast,
            Event::ConditionsUpdated(e) => self.conditions = Some(*e),
            Event::AlertsUpdated(e) => self.alerts = Some(*e),
            _ => return,
        }
        self.update();
    }
}
