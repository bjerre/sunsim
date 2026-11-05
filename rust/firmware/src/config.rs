//! What the C++ firmware takes from menuconfig (main/Kconfig.projbuild),
//! here from the build's environment, with Kconfig's defaults:
//! `SUNSIM_TZ="CET-1CEST,M3.5.0,M10.5.0/3" ./build.sh --release`. A new one
//! goes in config_names.rs too, so the build notices it change.

macro_rules! setting {
    ($name:literal, $default:literal) => {
        match option_env!($name) {
            Some(v) => v,
            None => $default,
        }
    };
}

/// POSIX TZ string for local time: "UTC0", "EST5EDT,M3.2.0/2,M11.1.0/2", ...
pub const TZ: &str = setting!("SUNSIM_TZ", "UTC0");
/// The Wi-Fi network to use while none is saved (console `wifi add`).
pub const WIFI_SSID: &str = setting!("SUNSIM_WIFI_SSID", "");
pub const WIFI_PASSWORD: &str = setting!("SUNSIM_WIFI_PASSWORD", "");
/// The status display ("y"), and whether the board is upside down.
pub const DISPLAY: &str = setting!("SUNSIM_DISPLAY", "y");
pub const DISPLAY_FLIP: &str = setting!("SUNSIM_DISPLAY_FLIP", "n");
/// Decimal degrees, positive = North / East.
pub const LATITUDE_DEG: &str = setting!("SUNSIM_LATITUDE_DEG", "0.0");
pub const LONGITUDE_DEG: &str = setting!("SUNSIM_LONGITUDE_DEG", "0.0");
/// Boot in fast-day mode ("y"), a loop of this many real seconds.
pub const DEBUG_FAST_DAY: &str = setting!("SUNSIM_DEBUG_FAST_DAY", "n");
pub const DEBUG_DAY_SECONDS: &str = setting!("SUNSIM_DEBUG_DAY_SECONDS", "120");


/// The program settings the build sets: the location.
pub fn default_program() -> ev::ProgramConfig {
    ev::ProgramConfig {
        latitude_deg: LATITUDE_DEG.parse().expect("SUNSIM_LATITUDE_DEG is a number"),
        longitude_deg: LONGITUDE_DEG.parse().expect("SUNSIM_LONGITUDE_DEG is a number"),
        ..ev::ProgramConfig::default()
    }
}

/// How the clock boots: fast-day's loop, if it's on.
pub fn fast_day() -> Option<ev::Duration> {
    (DEBUG_FAST_DAY == "y")
        .then(|| ev::Duration::secs(DEBUG_DAY_SECONDS.parse().expect("SUNSIM_DEBUG_DAY_SECONDS is a number")))
}

/// The build's Wi-Fi network, for when none is saved.
pub fn wifi_fallback() -> platform::WifiNetwork {
    platform::WifiNetwork { ssid: WIFI_SSID.into(), password: WIFI_PASSWORD.into() }
}
