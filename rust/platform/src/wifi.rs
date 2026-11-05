//! The Wi-Fi networks the internet sync may use.

use crate::storage::Storage;

/// A saved network. An empty password: an open network.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct WifiNetwork {
    pub ssid: String,
    pub password: String,
}

pub const MAX_WIFI_NETWORKS: usize = 5;
/// wifi_sta_config_t's ssid and password, less the terminator.
pub const MAX_SSID_LEN: usize = 32 - 1;
pub const MAX_PASSWORD_LEN: usize = 64 - 1;

// Pre-multi-network keys (one network). Read as part of the list; moved
// into the numbered slots the first time the list is changed.
const LEGACY_SSID_KEY: &str = "wifi.ssid";
const LEGACY_PASSWORD_KEY: &str = "wifi.pass";

// Slot keys "wifi<n>.ssid" / "wifi<n>.pass".
fn slot_keys(slot: usize) -> (String, String) {
    (format!("wifi{slot}.ssid"), format!("wifi{slot}.pass"))
}

/// The networks to try, and whether they're the build's fallback.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct WifiNetworks {
    pub networks: Vec<WifiNetwork>,
    pub from_build: bool, // nothing saved; this is the fallback
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum WifiSaveResult {
    Added,
    Updated,
    Invalid,
    Full,
    StorageError,
}

/// Wi-Fi networks for the internet sync, e.g. home and work. Saved from the
/// console (`wifi add`), kept in NVS (as the C++ firmware does: slots, and
/// its one-network keys before them) so they survive reflashing. When none
/// are saved, the build's network (if it names one) is used.
pub struct WifiStore<S: Storage> {
    storage: S,
    fallback: Option<WifiNetwork>,
}

impl<S: Storage> WifiStore<S> {
    pub fn new(storage: S, fallback: Option<WifiNetwork>) -> Self {
        WifiStore { storage, fallback: fallback.filter(|n| !n.ssid.is_empty()) }
    }

    fn str(&self, key: &str) -> Option<String> {
        self.storage.get_str(key).ok().flatten().filter(|s| !s.is_empty())
    }

    fn read_saved(&self) -> Vec<WifiNetwork> {
        let mut saved: Vec<WifiNetwork> = (0..MAX_WIFI_NETWORKS)
            .filter_map(|i| {
                let (ssid_key, pass_key) = slot_keys(i);
                let ssid = self.str(&ssid_key)?;
                Some(WifiNetwork { ssid, password: self.str(&pass_key).unwrap_or_default() }) // absent = open network
            })
            .collect();
        if let Some(ssid) = self.str(LEGACY_SSID_KEY)
            && !saved.iter().any(|n| n.ssid == ssid)
            && saved.len() < MAX_WIFI_NETWORKS
        {
            saved.insert(0, WifiNetwork { ssid, password: self.str(LEGACY_PASSWORD_KEY).unwrap_or_default() });
        }
        saved
    }

    fn write_saved(&mut self, saved: &[WifiNetwork]) -> bool {
        let s = &mut self.storage;
        let written = (0..MAX_WIFI_NETWORKS).try_for_each(|i| {
            let (ssid_key, pass_key) = slot_keys(i);
            match saved.get(i) {
                Some(n) => s.set_str(&ssid_key, &n.ssid).and_then(|()| s.set_str(&pass_key, &n.password)),
                None => s.erase(&ssid_key).and_then(|()| s.erase(&pass_key)),
            }
        });
        // Migrated into the slots now.
        let erased = s.erase(LEGACY_SSID_KEY).and_then(|()| s.erase(LEGACY_PASSWORD_KEY));
        written.and(erased).and_then(|()| s.commit()).is_ok()
    }

    pub fn load(&self) -> WifiNetworks {
        let networks = self.read_saved();
        match &self.fallback {
            Some(fallback) if networks.is_empty() => {
                WifiNetworks { networks: vec![fallback.clone()], from_build: true }
            }
            _ => WifiNetworks { networks, from_build: false },
        }
    }

    /// Adds the network, or updates its password if the SSID is already saved.
    pub fn add(&mut self, ssid: &str, password: &str) -> WifiSaveResult {
        if ssid.is_empty() || ssid.len() > MAX_SSID_LEN || password.len() > MAX_PASSWORD_LEN {
            return WifiSaveResult::Invalid;
        }
        let mut saved = self.read_saved();
        let full = saved.len() >= MAX_WIFI_NETWORKS;
        let result = match saved.iter_mut().find(|n| n.ssid == ssid) {
            Some(n) => {
                n.password = password.to_string();
                WifiSaveResult::Updated
            }
            None if full => return WifiSaveResult::Full,
            None => {
                saved.push(WifiNetwork { ssid: ssid.to_string(), password: password.to_string() });
                WifiSaveResult::Added
            }
        };
        if self.write_saved(&saved) { result } else { WifiSaveResult::StorageError }
    }

    /// False if no saved network has that SSID (or storage failed).
    pub fn remove(&mut self, ssid: &str) -> bool {
        let mut saved = self.read_saved();
        let before = saved.len();
        saved.retain(|n| n.ssid != ssid);
        saved.len() != before && self.write_saved(&saved)
    }

    pub fn clear(&mut self) {
        self.write_saved(&[]);
    }
}
