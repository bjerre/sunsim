//! The platform's Internet on the board: Wi-Fi sessions (sharing the C6's
//! radio with Zigbee), SNTP, and HTTPS checked against the certificate
//! bundle. A port of the C++ platform's wifi.cppm and internet_sync.cpp's
//! calls into ESP-IDF; when and what to fetch is platform::internet's.

use std::time::Duration;

use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::modem::Modem;
use esp_idf_svc::http::Method;
use esp_idf_svc::http::client::{Configuration as HttpConfiguration, EspHttpConnection};
use esp_idf_svc::nvs::EspDefaultNvsPartition;
use esp_idf_svc::sntp::{EspSntp, SyncStatus};
use esp_idf_svc::sys::{EspError, esp_coex_wifi_i154_enable, esp_crt_bundle_attach};
use esp_idf_svc::wifi::{AuthMethod, BlockingWifi, ClientConfiguration, Configuration, EspWifi};
use platform::{Internet, WifiNetwork, WifiStore};

use crate::{clock, config, storage};

const TAG: &str = "WIFI";

const MAX_RETRIES: u32 = 3; // per network, before moving on to the next

/// Long: while Zigbee shares the radio (busiest in fast-day mode, with light
/// commands every second) requests get lost. lwIP resends after 15, 30, then
/// 60 s, so this allows four tries.
const SNTP_TIMEOUT: Duration = Duration::from_secs(120);

const HTTP_TIMEOUT: Duration = Duration::from_secs(30); // slow while Zigbee shares the radio

pub struct EspInternet {
    wifi: BlockingWifi<EspWifi<'static>>,
    networks: WifiStore<storage::Nvs>,
}

impl EspInternet {
    pub fn new(modem: Modem<'static>, sysloop: EspSystemEventLoop, nvs: EspDefaultNvsPartition) -> Result<Self, EspError> {
        let wifi = EspWifi::new(modem, sysloop.clone(), Some(nvs.clone()))?;
        // Wi-Fi and Zigbee share the C6's one radio. Without this, Wi-Fi gets
        // no airtime once Zigbee runs: a scan finds no networks at all.
        // SAFETY: Wi-Fi is initialized (EspWifi::new).
        unsafe { esp_idf_svc::sys::esp!(esp_coex_wifi_i154_enable())? };
        Ok(EspInternet {
            wifi: BlockingWifi::wrap(wifi, sysloop)?,
            networks: WifiStore::new(storage::Nvs::open(nvs)?, Some(config::wifi_fallback())),
        })
    }

    // Saved networks to try, best first: those seen in a scan by signal
    // strength, then any not seen (a hidden SSID doesn't show in scans).
    fn candidates(&mut self, saved: Vec<WifiNetwork>) -> Vec<WifiNetwork> {
        if saved.len() == 1 {
            return saved; // nothing to choose between; skip the scan
        }
        let mut aps = match self.wifi.scan() {
            Ok(aps) => aps,
            Err(_) => {
                log::warn!(target: TAG, "Scan failed; trying saved networks in order");
                return saved;
            }
        };
        aps.sort_by_key(|ap| std::cmp::Reverse(ap.signal_strength));
        let mut seen: Vec<WifiNetwork> = Vec::new();
        for ap in &aps {
            if let Some(n) = saved.iter().find(|n| n.ssid == ap.ssid.as_str())
                && !seen.iter().any(|s| s.ssid == n.ssid)
            {
                log::info!(target: TAG, "In range: \"{}\" ({} dBm)", n.ssid, ap.signal_strength);
                seen.push(n.clone());
            }
        }
        if seen.is_empty() {
            log::warn!(target: TAG, "No saved network seen in the scan; trying each anyway (it may be hidden)");
            // What *is* around, to tell "out of range" from a typo or a 5
            // GHz-only network (the C6 is 2.4 GHz only).
            log::info!(target: TAG, "Scan found {} network(s){}", aps.len(), if aps.is_empty() { "" } else { ", strongest first:" });
            for ap in aps.iter().take(8) {
                log::info!(target: TAG, "  \"{}\" ({} dBm, channel {})", ap.ssid, ap.signal_strength, ap.channel);
            }
            return saved;
        }
        seen
    }

    fn try_network(&mut self, network: &WifiNetwork) -> bool {
        let (Ok(ssid), Ok(password)) = (network.ssid.as_str().try_into(), network.password.as_str().try_into()) else {
            log::warn!(target: TAG, "\"{}\": SSID or password too long", network.ssid);
            return false;
        };
        // AuthMethod::None: no minimum, so whatever the network uses (the password goes along).
        let conf = Configuration::Client(ClientConfiguration { ssid, password, auth_method: AuthMethod::None, ..Default::default() });
        if let Err(e) = self.wifi.set_configuration(&conf) {
            log::warn!(target: TAG, "\"{}\": {e}", network.ssid);
            return false;
        }
        log::info!(target: TAG, "Connecting to \"{}\"", network.ssid);
        for attempt in 1..=MAX_RETRIES + 1 {
            match self.wifi.connect().and_then(|()| self.wifi.wait_netif_up()) {
                Ok(()) => return true,
                Err(e) => log::info!(target: TAG, "  attempt {attempt} failed ({e})"),
            }
        }
        log::warn!(target: TAG, "Could not connect to \"{}\"", network.ssid);
        let _ = self.wifi.disconnect();
        false
    }
}

impl Internet for EspInternet {
    fn connect(&mut self) -> bool {
        let saved = self.networks.load().networks;
        if saved.is_empty() {
            log::warn!(target: TAG, "No Wi-Fi network configured (use `wifi add <ssid> <password>`)");
            return false;
        }
        if let Err(e) = self.wifi.start() {
            log::error!(target: TAG, "Failed to start Wi-Fi: {e}");
            return false;
        }
        for network in self.candidates(saved) {
            if self.try_network(&network) {
                return true;
            }
        }
        log::warn!(target: TAG, "No Wi-Fi network reachable");
        false
    }

    fn disconnect(&mut self) {
        let _ = self.wifi.disconnect();
        let _ = self.wifi.stop();
    }

    fn sync_clock(&mut self) -> Result<(), String> {
        let sntp = EspSntp::new_default().map_err(|e| format!("no SNTP: {e}"))?; // pool.ntp.org
        let deadline = std::time::Instant::now() + SNTP_TIMEOUT;
        while sntp.get_sync_status() != SyncStatus::Completed {
            if std::time::Instant::now() >= deadline {
                return Err("no time server answered".into());
            }
            std::thread::sleep(Duration::from_millis(500));
        }
        Ok(()) // dropping it stops SNTP
    }

    fn get(&mut self, url: &str, headers: &[(&str, &str)], sink: &mut dyn FnMut(&[u8])) -> Result<(), String> {
        let config = HttpConfiguration {
            crt_bundle_attach: Some(esp_crt_bundle_attach),
            timeout: Some(HTTP_TIMEOUT),
            ..Default::default()
        };
        let mut http = EspHttpConnection::new(&config).map_err(|_| "no HTTP client".to_string())?;
        http.initiate_request(Method::Get, url, headers)
            .and_then(|()| http.initiate_response())
            .map_err(|_| format!("could not reach {url}"))?;
        let status = http.status();
        if status != 200 {
            return Err(format!("HTTP {status} from {url}"));
        }
        let mut chunk = [0u8; 512];
        loop {
            match http.read(&mut chunk) {
                Ok(0) => return Ok(()),
                Ok(n) => sink(&chunk[..n]),
                Err(_) => return Err("the reply was cut off".into()),
            }
        }
    }

    fn wall(&self) -> ev::SysSeconds {
        clock::wall()
    }
    fn boot(&self) -> ev::Instant {
        clock::now()
    }
    fn utc_offset(&self, at: ev::SysSeconds) -> ev::Duration {
        clock::utc_offset(at)
    }
}
