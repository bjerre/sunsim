//! A runtime installation profile, kept in NVS: uploaded over the console a
//! chunk at a time, checked whole before it's saved, and loaded at boot (a
//! missing or invalid one leaves the built-in installation). As the C++
//! platform's installation_profile.cppm keeps it: key "install".

use ev::{Installation, parse_profile};

use crate::storage::Storage;

const TAG: &str = "PROFILE";
const KEY: &str = "install";

/// The largest profile: what fits NVS's string comfortably.
pub const MAX_PROFILE_BYTES: usize = 3072;

pub struct ProfileStore<S: Storage> {
    storage: S,
    upload: Option<(usize, Vec<u8>)>, // the size announced, what's come so far
}

impl<S: Storage> ProfileStore<S> {
    pub fn new(storage: S) -> Self {
        ProfileStore { storage, upload: None }
    }

    pub fn storage(&self) -> &S {
        &self.storage
    }

    /// The saved profile, checked; None (logged) if there's none or it's invalid.
    pub fn load(&self) -> Option<Installation> {
        let text = match self.storage.get_str(KEY) {
            Ok(Some(text)) => text,
            Ok(None) => return None,
            Err(e) => {
                log::warn!(target: TAG, "Saved profile is unreadable ({e}); using the built-in installation");
                return None;
            }
        };
        match parse_profile(&text) {
            Ok(installation) => {
                log::info!(target: TAG, "Loaded the runtime installation profile");
                Some(installation)
            }
            Err(why) => {
                log::warn!(target: TAG, "Saved profile is invalid ({why}); using the built-in installation");
                None
            }
        }
    }

    /// A new upload of `size` bytes (1-MAX_PROFILE_BYTES), replacing any unfinished.
    pub fn begin_upload(&mut self, size: usize) -> bool {
        if !(1..=MAX_PROFILE_BYTES).contains(&size) {
            return false;
        }
        self.upload = Some((size, Vec::with_capacity(size)));
        true
    }

    /// The next `bytes`, at `offset`: in order, within the size, no NULs.
    pub fn append(&mut self, offset: usize, bytes: &[u8]) -> bool {
        let Some((size, received)) = &mut self.upload else { return false };
        if offset != received.len() || bytes.is_empty() || bytes.len() > *size - received.len() || bytes.contains(&0) {
            return false;
        }
        received.extend_from_slice(bytes);
        true
    }

    /// The whole upload, checked, saved; or why not (the saved one stays).
    pub fn commit(&mut self) -> Result<(), String> {
        let (size, bytes) = self.upload.take().ok_or("no upload")?;
        if bytes.len() != size {
            return Err(format!("{} of {size} bytes arrived", bytes.len()));
        }
        let text = String::from_utf8(bytes).map_err(|_| "the profile isn't UTF-8".to_string())?;
        parse_profile(&text)?;
        self.storage
            .set_str(KEY, &text)
            .and_then(|()| self.storage.commit())
            .map_err(|e| format!("could not save it: {e}"))
    }

    /// Forgets the saved profile.
    pub fn clear(&mut self) -> bool {
        self.storage.erase(KEY).and_then(|()| self.storage.commit()).is_ok()
    }
}
