//! Everything that needs the internet.

use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::{Arc, Mutex};

use ev::*;
use weather::{AlertsReader, ConditionsReader, ForecastReader};

const TAG: &str = "INTERNET";

const RETRY_DELAY: Duration = Duration::minutes(10);

/// An unsynced clock starts at 1970; a synced one is well past this.
const CLOCK_SET_AFTER: SysSeconds = SysSeconds(days_from_civil(2024, 1, 1) * 86_400);

/// What the internet sync needs of the board. Its sessions: `connect()` brings
/// Wi-Fi up (false if no network could be reached), and `disconnect()`
/// takes it down again, whatever connect() said.
pub trait Internet {
    fn connect(&mut self) -> bool;
    fn disconnect(&mut self);
    /// Sets the wall clock from a time server.
    fn sync_clock(&mut self) -> Result<(), String>;
    /// GET over HTTPS (checked against the certificate bundle), handing the
    /// body to `sink` a chunk at a time as it arrives; or why not (the sink
    /// may have seen part of the body).
    fn get(&mut self, url: &str, headers: &[(&str, &str)], sink: &mut dyn FnMut(&[u8])) -> Result<(), String>;
    /// The wall clock, UTC.
    fn wall(&self) -> SysSeconds;
    /// Steady time since boot.
    fn boot(&self) -> Instant;
    /// The local UTC offset at `at`.
    fn utc_offset(&self, at: SysSeconds) -> Duration;
}

/// A coordinate as the services like it: at most four decimals, no trailing
/// zeros (NWS redirects "44.9800" to "44.98").
pub fn coordinate(degrees: f64) -> String {
    let text = format!("{degrees:.4}");
    text.trim_end_matches('0').trim_end_matches('.').to_string()
}

/// Open-Meteo's hourly cloud cover (free, no key), yesterday through the day
/// after tomorrow in UTC, so every local hour of today and tomorrow is
/// covered whatever the time zone.
pub fn forecast_url(latitude_deg: f64, longitude_deg: f64) -> String {
    format!(
        "https://api.open-meteo.com/v1/forecast?latitude={}&longitude={}\
         &hourly=cloud_cover&timeformat=unixtime&timezone=GMT&past_days=1&forecast_days=3",
        coordinate(latitude_deg),
        coordinate(longitude_deg)
    )
}

/// Open-Meteo's `current`: 15-minute model data (in the US, HRRR's).
pub fn conditions_url(latitude_deg: f64, longitude_deg: f64) -> String {
    format!(
        "https://api.open-meteo.com/v1/forecast?latitude={}&longitude={}&current=cloud_cover,weather_code&timeformat=unixtime",
        coordinate(latitude_deg),
        coordinate(longitude_deg)
    )
}

/// The National Weather Service's alerts in force for the location (US only;
/// elsewhere the reply is empty). It asks for a User-Agent.
pub fn alerts_url(latitude_deg: f64, longitude_deg: f64) -> String {
    format!(
        "https://api.weather.gov/alerts/active?status=actual&point={},{}",
        coordinate(latitude_deg),
        coordinate(longitude_deg)
    )
}
const ALERTS_HEADERS: [(&str, &str); 2] =
    [("User-Agent", "sunsim (ESP32 daylight simulator)"), ("Accept", "application/geo+json")];

/// The forecast's hours placed by local hour from `today`'s 00:00 (`to_local`
/// says what local time a UTC one is); None if none fell in the 48 hours.
pub fn place_forecast(
    hours: &[weather::Hour],
    today: LocalDays,
    to_local: impl Fn(SysSeconds) -> LocalSeconds,
) -> Option<CloudCover> {
    let mut forecast = CloudCover { day: Some(today), ..CloudCover::default() };
    let mut placed = 0;
    for h in hours {
        let slot = (to_local(h.start) - today).floor_secs().div_euclid(3600);
        if (0..CloudCover::HOURS as i64).contains(&slot) {
            forecast.percent[slot as usize] = h.cloud_percent;
            placed += 1;
        }
    }
    (placed > 0).then_some(forecast)
}

/// The jobs, in the order they run over one connection: HTTPS needs the clock.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Job {
    Clock,
    Forecast,
    Conditions,
    Alerts,
}

impl Job {
    pub const ALL: [Job; 4] = [Job::Clock, Job::Forecast, Job::Conditions, Job::Alerts];

    pub const fn name(self) -> &'static str {
        match self {
            Job::Clock => "clock",
            Job::Forecast => "forecast",
            Job::Conditions => "conditions",
            Job::Alerts => "alerts",
        }
    }

    /// None: daily, in the sync window; else every this long.
    const fn every(self) -> Option<Duration> {
        match self {
            Job::Clock | Job::Forecast => None,
            Job::Conditions | Job::Alerts => Some(Duration::minutes(15)),
        }
    }
}

#[derive(Clone, Copy, Default, Debug)]
struct JobState {
    last_try: Option<SysSeconds>,
    last_ok: Option<SysSeconds>,
}

/// The sync task's side: runs the jobs that are due, in one session.
pub struct Syncer<N: Internet> {
    post: Post,
    net: N,
    latitude_deg: f64,
    longitude_deg: f64,
    window: Arc<Mutex<DaySpan>>, // set from the app task (ConfigChanged)
    state: [JobState; 4],        // by Job::ALL's order
}

impl<N: Internet> Syncer<N> {
    pub fn new(post: Post, net: N, latitude_deg: f64, longitude_deg: f64) -> Self {
        Syncer {
            post,
            net,
            latitude_deg,
            longitude_deg,
            window: Arc::new(Mutex::new(ProgramConfig::default().sync)),
            state: [JobState::default(); 4],
        }
    }

    pub fn internet(&mut self) -> &mut N {
        &mut self.net
    }

    fn to_local(&self, t: SysSeconds) -> LocalSeconds {
        LocalSeconds(t.0) + self.net.utc_offset(t)
    }

    /// Runs the jobs that are due (every one if `everything`), in one Wi-Fi
    /// session; none due, no session.
    pub fn run(&mut self, everything: bool) {
        let now = self.net.wall();
        let chosen: Vec<usize> = (0..Job::ALL.len()).filter(|&i| everything || self.due(i, now)).collect();
        if chosen.is_empty() {
            return;
        }
        log::info!(target: TAG, "Syncing");
        let connected = self.net.connect();
        for i in chosen {
            let job = Job::ALL[i];
            let outcome = if !connected {
                Err("no Wi-Fi".to_string())
            } else if job != Job::Clock && self.net.wall() < CLOCK_SET_AFTER {
                Err("no clock yet (HTTPS can't check certificates)".to_string())
            } else {
                self.job(job)
            };
            match &outcome {
                Ok(()) => log::info!(target: TAG, "  {}: ok", job.name()),
                Err(why) => log::warn!(target: TAG, "  {}: failed ({why})", job.name()),
            }
            // Read after the job: the clock sync itself moves the clock.
            let tried = self.net.wall();
            self.state[i].last_try = Some(tried);
            if outcome.is_ok() {
                self.state[i].last_ok = Some(tried);
            }
        }
        self.net.disconnect();
    }

    fn job(&mut self, job: Job) -> Result<(), String> {
        let (lat, lon) = (self.latitude_deg, self.longitude_deg);
        match job {
            Job::Clock => {
                self.net.sync_clock()?;
                log::info!(target: TAG, "Clock synced: {} s after the epoch, UTC", self.net.wall().0);
                Ok(())
            }
            Job::Forecast => {
                let mut reader = ForecastReader::new();
                self.net.get(&forecast_url(lat, lon), &[], &mut |chunk| reader.feed(chunk))?;
                let today = self.to_local(self.net.wall()).day();
                let forecast =
                    place_forecast(&reader.hours(), today, |t| self.to_local(t)).ok_or("no forecast in the reply")?;
                log::info!(
                    target: TAG,
                    "Cloud cover; today 08:00 {}%, 12:00 {}%, 16:00 {}%",
                    forecast.percent[8],
                    forecast.percent[12],
                    forecast.percent[16]
                );
                self.post.send(CloudCoverUpdated { forecast });
                Ok(())
            }
            Job::Conditions => {
                let mut reader = ConditionsReader::new();
                self.net.get(&conditions_url(lat, lon), &[], &mut |chunk| reader.feed(chunk))?;
                let now = reader.conditions().ok_or("no conditions in the reply")?;
                match now.precip {
                    Precip::None => log::info!(target: TAG, "Now: {}% cloud, nothing falling", now.cloud_percent),
                    p => {
                        log::info!(target: TAG, "Now: {}% cloud, {} {}", now.cloud_percent, now.intensity.name(), p.name())
                    }
                }
                self.post.send(ConditionsUpdated { now, at: self.net.boot() });
                Ok(())
            }
            Job::Alerts => {
                let mut reader = AlertsReader::new();
                self.net.get(&alerts_url(lat, lon), &ALERTS_HEADERS, &mut |chunk| reader.feed(chunk))?;
                let alerts = reader.in_force(self.net.wall(), self.net.boot());
                if alerts.count == 0 {
                    log::info!(target: TAG, "No weather alerts");
                }
                for alert in alerts.list() {
                    log::info!(target: TAG, "Alert: {} ({})", alert.event(), alert.level.name());
                }
                self.post.send(AlertsUpdated { alerts, at: self.net.boot() });
                Ok(())
            }
        }
    }

    fn due(&self, i: usize, t: SysSeconds) -> bool {
        let state = self.state[i];
        let retry_ready = state.last_try.is_none_or(|last| t - last >= RETRY_DELAY);
        if t < CLOCK_SET_AFTER {
            return retry_ready; // never set: keep trying, window or not
        }
        if let Some(every) = Job::ALL[i].every() {
            return state.last_try.is_none_or(|last| t - last >= every);
        }
        let local = self.to_local(t);
        let since = local - local.day(); // since midnight
        let window = *self.window.lock().unwrap();
        if !window.contains(since) {
            return false;
        }
        // When today's window opened: done since then, nothing to do; else
        // the first try in it, or a retry.
        let opened = t - (since - window.start);
        if state.last_ok.is_some_and(|ok| ok >= opened) {
            return false;
        }
        state.last_try.is_none_or(|last| last < opened) || retry_ready
    }
}

/// Everything that needs the internet, batched into short Wi-Fi sessions
/// (Wi-Fi shares the 2.4 GHz radio with Zigbee). Each job has a cadence:
///
/// ```text
/// clock        SNTP                                      daily
/// forecast     Open-Meteo hourly cloud cover, today and  daily
///              tomorrow -> CloudCoverUpdated
/// conditions   Open-Meteo's weather now -> ConditionsUpdated   every 15 min
/// alerts       NWS's active alerts for the location -> AlertsUpdated   every 15 min
/// ```
///
/// Daily: once a day in the sync window (ProgramConfig, `config
/// syncwindow`, default 04:00-05:00), retrying every 10 minutes within it on
/// failure. Every job also runs at boot (start() waits for it, before the
/// Zigbee radio starts) and when asked (SyncRequested: console `sync`,
/// switching fast-day mode). A session runs whichever jobs are due, in the
/// order above; while the clock has never been set, every 10 minutes
/// regardless: the lights stay off without it. Everything runs on its own
/// task (TLS needs the stack); the replies are read as they stream in
/// (weather), whatever their size. This is the app task's side: it tells
/// the sync task the window, and when it's asked.
pub struct InternetSync {
    asked: mpsc::Sender<()>,
    window: Arc<Mutex<DaySpan>>,
}

impl InternetSync {
    /// Starts the sync task (`spawn` runs it: on the board, with the stack
    /// TLS needs) and waits for its first sync, the boot sync. The sync task
    /// ends when this is dropped.
    pub fn start<N: Internet + Send + 'static>(
        syncer: Syncer<N>,
        spawn: impl FnOnce(Box<dyn FnOnce() + Send>),
    ) -> Self {
        let (asked, requests) = mpsc::channel::<()>();
        let (booted, boot_done) = mpsc::channel::<()>();
        let window = syncer.window.clone();
        let mut syncer = syncer;
        spawn(Box::new(move || {
            syncer.run(true); // the boot sync
            let _ = booted.send(());
            loop {
                // A minute at a time, or at once when asked (once for however many asks).
                let asked = match requests.recv_timeout(std::time::Duration::from_secs(60)) {
                    Ok(()) => true,
                    Err(RecvTimeoutError::Timeout) => false,
                    Err(RecvTimeoutError::Disconnected) => return,
                };
                while requests.try_recv().is_ok() {}
                syncer.run(asked);
            }
        }));
        let _ = boot_done.recv();
        InternetSync { asked, window }
    }
}

impl Module for InternetSync {
    fn on(&mut self, e: &Event) {
        match e {
            Event::ConfigChanged(e) => *self.window.lock().unwrap() = e.config.sync,
            Event::SyncRequested(_) => {
                let _ = self.asked.send(());
            }
            _ => {}
        }
    }
}
