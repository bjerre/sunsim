//! The serial console's commands: a typed line in, the reply out. Commands
//! post requests as events and read state from the latest StatusSnapshot;
//! they never touch app state directly. A port of the C++ platform's
//! console.cppm; the firmware reads the lines (and echoes them) and prints
//! the replies.
//!
//! ```text
//! status                time, day length, next sunrise/sunset, mode, device links
//! roles                 list role assignments
//! pair <role>           next unassigned device to join gets <role>; opens the network
//! pair cancel           disarm a pending `pair`
//! unpair <role|group|all>   forget role assignment(s)
//! open [seconds]        open the network for joining (default 180)
//! config                show the settings (SETTINGS)
//! config <key> <value>  change one (`config x` lists what each takes)
//! intensity [<light|group|all> <1-100>]   per-light peak brightness, saved
//! minlevel [<light|group|all> <1-254>]    per-light minimum level while on, saved
//! tune [<light|group|all> <1-254> [kelvin] | save | off]   find each bulb's lowest visible level
//! press single|double|hold          simulate the button
//! fastday [on [seconds] | off]      the debug fast-day clock (a whole day in seconds)
//! sync                  sync the clock (and fetch internet data) now
//! weather               the weather now: cloud, rain or snow, alerts, and the weather lights
//! weather simulate <what>   pretend: rain|snow|... [heavy], advisory|watch|warning, or clear
//! wifi                  list saved Wi-Fi networks (the strongest in range is used)
//! wifi add <ssid> [pw]  add a network (up to 5), or change its password
//! wifi remove <ssid>    forget one; `wifi clear` forgets all
//! restart               reboot, keeping everything
//! factory_reset         forget all roles AND the Zigbee network, then restart
//! ```
//!
//! <role> is a device's name (WallA-WallE east to west, Work, Button); a
//! <group> is all of one kind (wall). Case doesn't matter.

use std::fmt::Write as _;
use std::sync::{Arc, Mutex};

use ev::*;

use crate::storage::Storage;
use crate::wifi::{MAX_WIFI_NETWORKS, WifiSaveResult, WifiStore};

/// The latest StatusSnapshot, for readers outside the app task (console
/// commands, the display task). Clones are the same slot.
#[derive(Clone, Default)]
pub struct StatusSlot(Arc<Mutex<Option<Arc<StatusSnapshot>>>>);

impl StatusSlot {
    /// None until the first status update (about a second after the app starts).
    pub fn get(&self) -> Option<Arc<StatusSnapshot>> {
        self.0.lock().unwrap().clone()
    }
}

impl Module for StatusSlot {
    fn on(&mut self, e: &Event) {
        if let Event::StatusChanged(e) = e {
            *self.0.lock().unwrap() = Some(e.snapshot.clone());
        }
    }
}

/// What the console needs of the board.
pub trait ConsoleBoard {
    /// Steady time since boot.
    fn now(&self) -> Instant;
    /// Reboots (on the board, it doesn't return).
    fn restart(&mut self);
}

/// A time after midnight (hours) as "HH:MM". 24:00 stays as is (a window
/// ending at midnight); anything later wraps into the next day.
pub fn clock_text(after_midnight: f64) -> String {
    let mut minutes = (after_midnight * 60.0).round() as i64;
    if minutes > 24 * 60 {
        minutes -= 24 * 60;
    }
    format!("{:02}:{:02}", minutes.div_euclid(60), minutes.rem_euclid(60))
}

/// A duration (hours) as "Xh MMm".
pub fn duration_text(hours: f64) -> String {
    let minutes = (hours * 60.0).round() as i64;
    format!("{}h {:02}m", minutes.div_euclid(60), minutes.rem_euclid(60))
}

/// A line split into words as esp_console splits it: at spaces, with
/// "double quotes" around words with spaces and \ escaping the next character.
pub fn split_words(line: &str) -> Vec<String> {
    let mut words = Vec::new();
    let mut word: Option<String> = None;
    let mut quoted = false;
    let mut chars = line.chars();
    while let Some(c) = chars.next() {
        match c {
            '\\' => word.get_or_insert_default().extend(chars.next()),
            '"' => {
                quoted = !quoted;
                word.get_or_insert_default();
            }
            c if c.is_whitespace() && !quoted => words.extend(word.take()),
            c => word.get_or_insert_default().push(c),
        }
    }
    words.extend(word);
    words
}

// A whole number from lo to hi, all of `text`: not "12x", "" or "-".
fn number_in(text: &str, lo: i64, hi: i64) -> Option<i64> {
    text.parse().ok().filter(|v| (lo..=hi).contains(v) && !text.starts_with('+'))
}

// What `weather simulate` takes for rain and snow, and the alerts.
const PRECIP_NAMES: [(&str, Precip); 5] = [
    ("drizzle", Precip::Drizzle),
    ("rain", Precip::Rain),
    ("freezing", Precip::FreezingRain),
    ("snow", Precip::Snow),
    ("thunder", Precip::Thunderstorm),
];
const ALERT_NAMES: [(&str, AlertLevel); 3] =
    [("advisory", AlertLevel::Advisory), ("watch", AlertLevel::Watch), ("warning", AlertLevel::Warning)];

const WEEKDAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

macro_rules! out {
    ($o:expr) => { $o.push('\n') };
    ($o:expr, $($arg:tt)*) => { { let _ = writeln!($o, $($arg)*); } };
}
macro_rules! put {
    ($o:expr, $($arg:tt)*) => { { let _ = write!($o, $($arg)*); } };
}

type Run<S, B> = fn(&mut Console<S, B>, &[&str], &mut String) -> i32;

struct Command<S: Storage, B: ConsoleBoard> {
    name: &'static str,
    hint: &'static str,
    help: &'static str,
    run: Run<S, B>,
}

/// The console's commands, on the bus's Post, the status slot, the saved
/// Wi-Fi networks and the board.
pub struct Console<S: Storage, B: ConsoleBoard> {
    post: Post,
    status: StatusSlot,
    wifi: WifiStore<S>,
    board: B,
    /// `fastday on`'s loop by default, real seconds.
    day_seconds: i64,
}

impl<S: Storage, B: ConsoleBoard> Console<S, B> {
    pub const PROMPT: &'static str = "sunsim> ";

    pub fn new(post: Post, status: StatusSlot, wifi: WifiStore<S>, board: B, day_seconds: i64) -> Self {
        Console { post, status, wifi, board, day_seconds }
    }

    pub fn board(&mut self) -> &mut B {
        &mut self.board
    }

    fn commands() -> [Command<S, B>; 17] {
        [
            Command { name: "help", hint: "", help: "Print the list of registered commands", run: Self::help },
            Command {
                name: "status",
                hint: "",
                help: "Time, day length, next sunrise/sunset, mode and device links",
                run: Self::status,
            },
            Command { name: "roles", hint: "", help: "List role assignments", run: Self::roles },
            Command {
                name: "pair",
                hint: "<role|cancel>",
                help: "Give the next unassigned device to join this role, and open the network",
                run: Self::pair,
            },
            Command {
                name: "unpair",
                hint: "<role|group|all>",
                help: "Forget a role's device, or a group's (they can then be re-paired)",
                run: Self::unpair,
            },
            Command { name: "open", hint: "[seconds]", help: "Open the network for joining", run: Self::open },
            Command {
                name: "config",
                hint: "[<setting> <value>]",
                help: "Show the settings, or change one (`config x` lists what each takes)",
                run: Self::config,
            },
            Command {
                name: "intensity",
                hint: "[<light|group|all> <1-100>]",
                help: "Show or set each light's peak brightness, 1-100%",
                run: Self::intensity,
            },
            Command {
                name: "minlevel",
                hint: "[<light|group|all> <1-254>]",
                help: "Show or set each light's minimum level while on, 1-254 (find them with tune)",
                run: Self::min_level,
            },
            Command {
                name: "tune",
                hint: "[<light|group|all> <1-254> [kelvin] | save | off]",
                help: "Tuning mode: hold bulbs steady to find each one's lowest visible level",
                run: Self::tune,
            },
            Command {
                name: "press",
                hint: "single|double|hold",
                help: "Simulate the button: single = resume daylight, double = red alert, hold = all off",
                run: Self::press,
            },
            Command {
                name: "fastday",
                hint: "[on [seconds] | off]",
                help: "Debug: run a whole active window every few seconds, or back to real time",
                run: Self::fast_day,
            },
            Command {
                name: "sync",
                hint: "",
                help: "Sync the clock (and fetch internet data) now, over Wi-Fi",
                run: Self::sync,
            },
            Command {
                name: "weather",
                hint: "[simulate drizzle|rain|freezing|snow|thunder [light|heavy] | advisory|watch|warning | clear]",
                help: "The weather now and the weather lights; or pretend, to see the lights",
                run: Self::weather,
            },
            Command {
                name: "wifi",
                hint: "[add <ssid> [password] | remove <ssid> | clear]",
                help: "List, add or remove the Wi-Fi networks used for the internet sync (e.g. home and work)",
                run: Self::wifi,
            },
            Command {
                name: "restart",
                hint: "",
                help: "Reboot (keeps roles, network and Wi-Fi settings)",
                run: Self::restart,
            },
            Command {
                name: "factory_reset",
                hint: "",
                help: "Forget all roles AND the Zigbee network, then restart",
                run: Self::factory_reset,
            },
        ]
    }

    /// Runs a typed line; what to print in reply (with esp_console's words
    /// for an unknown command or one that failed, which ../../sunsim reads).
    pub fn line(&mut self, line: &str) -> String {
        let words = split_words(line);
        let args: Vec<&str> = words.iter().map(String::as_str).collect();
        let mut o = String::new();
        let Some(&name) = args.first() else { return o };
        match Self::commands().into_iter().find(|c| c.name == name) {
            None => out!(o, "Unrecognized command"),
            Some(command) => {
                let code = (command.run)(self, &args, &mut o);
                if code != 0 {
                    out!(o, "Command returned non-zero error code: 0x{code:x} (ERROR)");
                }
            }
        }
        o
    }

    fn help(&mut self, _: &[&str], o: &mut String) -> i32 {
        for c in Self::commands() {
            out!(o, "{} {}", c.name, c.hint);
            out!(o, "  {}", c.help);
            out!(o);
        }
        0
    }

    // The latest status, or None after saying it's still starting up.
    fn snapshot(&self, o: &mut String) -> Option<Arc<StatusSnapshot>> {
        let s = self.status.get();
        if s.is_none() {
            out!(o, "still starting up; try again in a moment");
        }
        s
    }

    fn role_names(o: &mut String) {
        put!(o, "roles:");
        for role in ROLES {
            put!(o, " {}", role_name(role));
        }
        out!(o);
    }

    // The role named by `arg`, or None after saying why not.
    fn role_arg(arg: &str, o: &mut String) -> Option<Role> {
        let role = parse_role(arg);
        if role.is_none() {
            out!(o, "unknown role '{arg}'");
            Self::role_names(o);
        }
        role
    }

    // The role, group or "all" named by `arg`, or None after saying why not.
    fn selection_arg(arg: &str, o: &mut String) -> Option<Selection> {
        let selection = parse_selection(arg);
        if selection.is_none() {
            out!(o, "unknown role or group '{arg}'");
            Self::role_names(o);
            put!(o, "groups:");
            for g in LAYOUT {
                put!(o, " {}", g.name);
            }
            out!(o, " (or all)");
        }
        selection
    }

    // ---- roles / pair / unpair / open -------------------------------------

    fn roles(&mut self, _: &[&str], o: &mut String) -> i32 {
        let Some(st) = self.snapshot(o) else { return 1 };
        for role in ROLES {
            let d = st.records[role];
            if d.valid {
                out!(
                    o,
                    "{:<6} ext=0x{:016x} short=0x{:04x} ep={}",
                    role_name(role),
                    d.ext_addr,
                    d.short_addr,
                    d.endpoint
                );
            } else {
                out!(o, "{:<6} (unassigned)", role_name(role));
            }
        }
        if let Some(armed) = st.armed {
            out!(o, "Armed: next joining device becomes {}", role_name(armed));
        }
        0
    }

    fn pair(&mut self, args: &[&str], o: &mut String) -> i32 {
        if args.len() != 2 {
            out!(o, "usage: pair <role>|cancel");
            Self::role_names(o);
            return 1;
        }
        if args[1] == "cancel" {
            self.post.send(PairCanceled);
            return 0;
        }
        let Some(role) = Self::role_arg(args[1], o) else { return 1 };
        self.post.send(PairRequested { role });
        0
    }

    fn unpair(&mut self, args: &[&str], o: &mut String) -> i32 {
        if args.len() != 2 {
            out!(o, "usage: unpair <role|group|all>");
            Self::role_names(o);
            return 1;
        }
        let Some(roles) = Self::selection_arg(args[1], o) else { return 1 };
        self.post.send(UnpairRequested { roles });
        0
    }

    fn open(&mut self, args: &[&str], o: &mut String) -> i32 {
        let secs = if args.len() == 2 { number_in(args[1], 1, 254) } else { Some(DEFAULT_OPEN_SECS as i64) };
        let Some(secs) = secs else {
            out!(o, "seconds must be 1-254");
            return 1;
        };
        self.post.send(OpenNetwork { secs: secs as u8 });
        0
    }

    // ---- config ------------------------------------------------------------

    // Every setting is in SETTINGS: what it's called, takes and means.

    fn print_setting(setting: &Setting, c: &ProgramConfig, o: &mut String) {
        out!(o, "  {:<11} {:<14} {}", setting.name, setting.show(c), setting.help);
    }

    fn config(&mut self, args: &[&str], o: &mut String) -> i32 {
        let Some(st) = self.snapshot(o) else { return 1 };
        let mut c = st.config;
        if args.len() == 1 {
            for setting in SETTINGS {
                Self::print_setting(setting, &c, o);
            }
            return 0;
        }
        let Some(setting) = (args.len() == 3).then(|| find_setting(args[1])).flatten() else {
            out!(o, "usage: config [<setting> <value>]");
            for setting in SETTINGS {
                out!(o, "  {:<11} {}", setting.name, setting.syntax());
            }
            return 1;
        };
        if let Err(why) = setting.set(&mut c, args[2]) {
            out!(o, "{why}");
            return 1;
        }
        self.post.send(SetConfig { config: c });
        Self::print_setting(setting, &c, o);
        0
    }

    // ---- intensity / minlevel / tune ---------------------------------------

    fn print_levels(levels: &PerRole<u8>, unit: &str, o: &mut String) {
        for role in lights() {
            out!(o, "  {:<6} {:3}{unit}", role_name(role), levels[role]);
        }
    }

    fn intensity(&mut self, args: &[&str], o: &mut String) -> i32 {
        let Some(st) = self.snapshot(o) else { return 1 };
        if args.len() == 1 {
            Self::print_levels(&st.peaks, "%", o);
            return 0;
        }
        let Some(percent) = (args.len() == 3).then(|| number_in(args[2], 1, 100)).flatten() else {
            out!(o, "usage: intensity [<light|group|all> <1-100>]   e.g. intensity WallC 80, intensity wall 60");
            return 1;
        };
        let Some(chosen) = Self::selection_arg(args[1], o) else { return 1 };
        if !lights().any(|r| selects(chosen, r)) {
            out!(o, "'{}' isn't a light", args[1]);
            return 1;
        }
        self.post.send(SetIntensity { lights: chosen, percent: percent as u8 });
        let mut peaks = st.peaks;
        for role in lights().filter(|&r| selects(chosen, r)) {
            peaks[role] = percent as u8;
        }
        Self::print_levels(&peaks, "%", o);
        0
    }

    fn min_level(&mut self, args: &[&str], o: &mut String) -> i32 {
        let Some(st) = self.snapshot(o) else { return 1 };
        if args.len() == 1 {
            Self::print_levels(&st.min_levels, "", o);
            return 0;
        }
        let Some(level) = (args.len() == 3).then(|| number_in(args[2], 1, MAX_LEVEL as i64)).flatten() else {
            out!(o, "usage: minlevel [<light|group|all> <1-254>]   (find them with `tune`)");
            return 1;
        };
        let Some(chosen) = Self::selection_arg(args[1], o) else { return 1 };
        self.post.send(SetMinLevel { lights: chosen, level: level as u8 });
        let mut levels = st.min_levels;
        for role in lights().filter(|&r| selects(chosen, r)) {
            levels[role] = level as u8;
        }
        Self::print_levels(&levels, "", o);
        0
    }

    fn tune(&mut self, args: &[&str], o: &mut String) -> i32 {
        let Some(st) = self.snapshot(o) else { return 1 };
        let tuning = st.zones.0.iter().any(|z| z.mode == Mode::Tuning);
        let tune = |post: &Post| post.send(ModeRequested { mode: Mode::Tuning, zone: None });
        match args {
            [_] => {
                if !tuning {
                    tune(&self.post);
                }
                out!(
                    o,
                    "tuning: every bulb steady at its minimum level, in sunset amber. Raise a bulb until it\n\
                     shows, then `tune save`; `tune off` (or a button press) returns to daylight.\n  \
                     tune <light|group|all> <1-254> [kelvin]\nminimum levels now:"
                );
                Self::print_levels(&st.min_levels, "", o);
                0
            }
            [_, "off"] => {
                self.post.send(ModeRequested { mode: Mode::Daylight, zone: None });
                0
            }
            [_, "save"] if !tuning => {
                out!(o, "not tuning: nothing to save (start with `tune`)");
                1
            }
            [_, "save"] => {
                self.post.send(TuneSave);
                out!(o, "saved as the minimum levels (see `minlevel`)");
                0
            }
            [_, which, level] | [_, which, level, _] => {
                let kelvin = args.get(3).map(|k| number_in(k, 1700, 6500)); // Some(None): given, but not 1700-6500
                let (Some(level), false) = (number_in(level, 1, MAX_LEVEL as i64), matches!(kelvin, Some(None))) else {
                    out!(o, "usage: tune [<light|group|all> <1-254> [1700-6500 kelvin] | save | off]");
                    return 1;
                };
                let Some(chosen) = Self::selection_arg(which, o) else { return 1 };
                if !tuning {
                    tune(&self.post);
                }
                self.post.send(TuneLights {
                    lights: chosen,
                    level: level as u8,
                    kelvin: kelvin.flatten().map(|k| k as u16),
                });
                0
            }
            _ => {
                out!(o, "usage: tune [<light|group|all> <1-254> [1700-6500 kelvin] | save | off]");
                1
            }
        }
    }

    // ---- press / fastday / sync --------------------------------------------

    // Posts the same event the real button does, so this exercises the whole
    // press -> mode -> lights path.
    fn press(&mut self, args: &[&str], o: &mut String) -> i32 {
        let press = match args {
            [_, "single"] => Press::Single,
            [_, "double"] => Press::Double,
            [_, "hold"] => Press::Hold,
            _ => {
                out!(o, "usage: press single|double|hold");
                return 1;
            }
        };
        let Some(button) = ROLES.into_iter().find(|&r| is_button(r)) else {
            out!(o, "there's no button in the layout");
            return 1;
        };
        self.post.send(ButtonPressed { button, press, source: Source::Console, at: self.board.now() });
        0
    }

    fn fast_day(&mut self, args: &[&str], o: &mut String) -> i32 {
        match args {
            [_] => {
                let Some(st) = self.snapshot(o) else { return 1 };
                out!(o, "fast-day {}", if st.time.simulated { "on" } else { "off" });
                0
            }
            [_, "off"] => {
                self.post.send(SetFastDay { on: false, ..SetFastDay::default() });
                0
            }
            [_, "on"] | [_, "on", _] => {
                let secs = args.get(2).map_or(Some(self.day_seconds), |s| number_in(s, 10, 3600));
                let Some(secs) = secs else {
                    out!(o, "seconds per day must be 10-3600");
                    return 1;
                };
                self.post.send(SetFastDay { on: true, day_length: Duration::secs(secs) });
                0
            }
            _ => {
                out!(
                    o,
                    "usage: fastday [on [seconds] | off]   (on: one active window every {} s by default)",
                    self.day_seconds
                );
                1
            }
        }
    }

    fn sync(&mut self, _: &[&str], o: &mut String) -> i32 {
        self.post.send(SyncRequested);
        out!(o, "syncing in the background (see the INTERNET log lines)");
        0
    }

    // ---- weather -----------------------------------------------------------

    // "WallB,WallD", or "none".
    fn lights_text(set: RoleSet) -> String {
        let names: Vec<_> = lights().filter(|&r| set.contains(r)).map(role_name).collect();
        if names.is_empty() { "none".into() } else { names.join(",") }
    }

    fn weather(&mut self, args: &[&str], o: &mut String) -> i32 {
        let Some(st) = self.snapshot(o) else { return 1 };
        match args {
            [_] => {
                match st.sky.conditions {
                    Some(c) if c.precip == Precip::None => {
                        out!(o, "now      {}% cloud, nothing falling", c.cloud_percent)
                    }
                    Some(c) => {
                        out!(o, "now      {}% cloud, {} {}", c.cloud_percent, c.intensity.name(), c.precip.name())
                    }
                    None => out!(
                        o,
                        "now      no live conditions (none fetched in the last {} min)",
                        WEATHER_FRESH_FOR.whole_minutes()
                    ),
                }
                if st.sky.alerts.count == 0 {
                    out!(o, "alerts   none");
                }
                for alert in st.sky.alerts.list() {
                    let left = (alert.until - st.at).whole_minutes();
                    out!(o, "alert    {} ({}), {left} min left", alert.event(), alert.level.name());
                }
                Self::print_setting(find_setting("weather").expect("a weather setting"), &st.config, o);
                out!(o, "precip   {}", Self::lights_text(WEATHER_LIGHTS.precip));
                out!(o, "alerts   {}", Self::lights_text(WEATHER_LIGHTS.alerts));
                0
            }
            [_, "simulate", what] => self.simulate_weather(&st, what, "", o),
            [_, "simulate", what, how] => self.simulate_weather(&st, what, how, o),
            _ => {
                out!(o, "usage: weather [simulate <what>]");
                1
            }
        }
    }

    // Posts what InternetSync would: it lasts until the next real fetch
    // (within 15 minutes), or `weather simulate clear`.
    fn simulate_weather(&mut self, st: &StatusSnapshot, what: &str, how: &str, o: &mut String) -> i32 {
        let now = self.board.now();
        let cloud = st.sky.conditions.map_or(st.sky.cloud_percent.unwrap_or(0), |c| c.cloud_percent);
        if what == "clear" {
            self.post
                .send(ConditionsUpdated { now: Conditions { cloud_percent: cloud, ..Conditions::default() }, at: now });
            self.post.send(AlertsUpdated { alerts: Alerts::default(), at: now });
            return 0;
        }
        if let Some(&(_, precip)) = PRECIP_NAMES.iter().find(|(n, _)| *n == what) {
            let intensity = match how {
                "light" => Intensity::Light,
                "heavy" => Intensity::Heavy,
                _ => Intensity::Moderate,
            };
            self.post.send(ConditionsUpdated {
                now: Conditions { cloud_percent: cloud.max(90), precip, intensity },
                at: now,
            });
            return 0;
        }
        if let Some(&(_, level)) = ALERT_NAMES.iter().find(|(n, _)| *n == what) {
            let mut alert = Alert { level, until: now + Duration::hours(1), ..Alert::default() };
            alert.set_event(&format!("Simulated {}", level.name()));
            let mut alerts = Alerts::default();
            alerts.push(alert);
            self.post.send(AlertsUpdated { alerts, at: now });
            return 0;
        }
        out!(
            o,
            "usage: weather simulate drizzle|rain|freezing|snow|thunder [light|moderate|heavy]\n       \
             weather simulate advisory|watch|warning | clear\n(until the next real fetch, within 15 min)"
        );
        1
    }

    // ---- status ------------------------------------------------------------

    fn status(&mut self, _: &[&str], o: &mut String) -> i32 {
        let Some(st) = self.snapshot(o) else { return 1 };

        if st.time.valid {
            let minutes = st.time.since_midnight().whole_minutes();
            out!(
                o,
                "time    {} {:02}:{:02}{}",
                WEEKDAYS[st.time.weekday().0 as usize],
                minutes / 60,
                minutes % 60,
                if st.time.simulated { "  (simulated)" } else { "" }
            );
        } else {
            out!(o, "time    NOT SET - waiting for time sync (lights held off)");
        }

        let day = &st.day;
        out!(
            o,
            "day     {}  (sunrise {}, sunset {})",
            duration_text(day.day_length),
            clock_text(day.sunrise),
            clock_text(day.sunset)
        );
        out!(
            o,
            "next    {} in {} ({})",
            if day.next_is_sunrise { "sunrise" } else { "sunset" },
            duration_text(day.until_next),
            clock_text(day.next_event)
        );

        match st.sky.cloud_percent {
            Some(cloud) => out!(
                o,
                "sky     {cloud}% cloud ({}){}",
                if st.sky.cloud_live { "now" } else { "forecast" },
                if st.config.clouds_enabled { "" } else { " (clouds off)" }
            ),
            None => out!(o, "sky     no forecast yet (clear assumed)"),
        }
        if let Some(c) = st.sky.conditions.filter(|c| c.precip != Precip::None) {
            out!(o, "falling {} {}", c.intensity.name(), c.precip.name());
        }
        for alert in st.sky.alerts.list() {
            out!(o, "alert   {}", alert.event());
        }

        for zone in zone_ids() {
            let z = st.zones[zone];
            put!(o, "mode    ");
            if ZONE_COUNT > 1 {
                put!(o, "{}: ", spec_of(zone).name);
            }
            put!(o, "{}", z.mode.name());
            if z.mode == Mode::Daylight {
                put!(o, " - {}", if z.gate_open { "lights may be on (gate open)" } else { "lights off (gate shut)" });
            }
            out!(o);
        }

        for role in ROLES {
            let d = st.devices[role];
            put!(o, "  {:<6} ", role_name(role));
            match d.link {
                DeviceLink::Unpaired => {
                    out!(o, "-  not paired");
                    continue;
                }
                DeviceLink::Silent => put!(o, "o  silent"),
                DeviceLink::Online => put!(o, "*  online"),
            }
            if let Some(heard) = d.last_heard {
                put!(o, ", heard {}s ago", (st.at - heard).floor_secs());
            }
            if d.rssi != 0 {
                put!(o, ", rssi {}", d.rssi);
            }
            if !d.has_endpoint {
                put!(o, ", endpoint not found yet");
            }
            out!(o);
        }
        0
    }

    // ---- wifi / restart / factory_reset ------------------------------------

    // Wi-Fi networks live in their own NVS keys and are only read by the
    // sync task at connect time, so these go to the store directly.
    fn wifi(&mut self, args: &[&str], o: &mut String) -> i32 {
        match args {
            [_] => {
                let saved = self.wifi.load();
                if saved.networks.is_empty() {
                    out!(o, "no Wi-Fi networks; add one with: wifi add <ssid> [password]");
                    return 0;
                }
                if saved.from_build {
                    out!(o, "none saved; using the build's fallback:");
                }
                for (i, n) in saved.networks.iter().enumerate() {
                    out!(
                        o,
                        "  {}. \"{}\" ({})",
                        i + 1,
                        n.ssid,
                        if n.password.is_empty() { "open network" } else { "password set" }
                    );
                }
                out!(o, "the strongest one in range is used at each clock sync");
                0
            }
            [_, "add", ssid] | [_, "add", ssid, _] => {
                let password = args.get(3).copied().unwrap_or("");
                match self.wifi.add(ssid, password) {
                    WifiSaveResult::Added => {
                        out!(o, "added \"{ssid}\"; type `sync` to sync time now");
                        0
                    }
                    WifiSaveResult::Updated => {
                        out!(o, "updated the password for \"{ssid}\"");
                        0
                    }
                    WifiSaveResult::Invalid => {
                        out!(o, "not saved: SSID must be 1-32 chars, password at most 64");
                        1
                    }
                    WifiSaveResult::Full => {
                        out!(o, "not saved: already {MAX_WIFI_NETWORKS} networks; `wifi remove` one first");
                        1
                    }
                    WifiSaveResult::StorageError => {
                        out!(o, "not saved: storage error");
                        1
                    }
                }
            }
            [_, "remove", ssid] => {
                if !self.wifi.remove(ssid) {
                    out!(o, "no saved network called \"{ssid}\"");
                    return 1;
                }
                out!(o, "removed \"{ssid}\"");
                0
            }
            [_, "clear"] => {
                self.wifi.clear();
                out!(o, "all networks forgotten; falling back to the build's Wi-Fi settings");
                0
            }
            _ => {
                out!(
                    o,
                    "usage: wifi                          list saved networks\n       \
                     wifi add <ssid> [password]    add one (or change its password); up to {MAX_WIFI_NETWORKS}\n       \
                     wifi remove <ssid>            forget one\n       \
                     wifi clear                    forget all (falls back to the build's)\n\
                     quote an SSID with spaces: wifi add \"My Net\" secret"
                );
                1
            }
        }
    }

    fn restart(&mut self, _: &[&str], _: &mut String) -> i32 {
        self.board.restart();
        0
    }

    fn factory_reset(&mut self, _: &[&str], _: &mut String) -> i32 {
        self.post.send(FactoryReset);
        0
    }
}
