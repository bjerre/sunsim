//! What persisting needs of the board: NVS's typed keys, in one namespace.

use std::fmt::Display;

/// A namespace of typed values by key (at most 15 characters), as NVS keeps
/// them: a value is read back only at the width it was written.
pub trait Storage {
    type Error: Display;

    fn get_u8(&self, key: &str) -> Result<Option<u8>, Self::Error>;
    fn get_u16(&self, key: &str) -> Result<Option<u16>, Self::Error>;
    fn get_u64(&self, key: &str) -> Result<Option<u64>, Self::Error>;
    fn set_u8(&mut self, key: &str, value: u8) -> Result<(), Self::Error>;
    fn set_u16(&mut self, key: &str, value: u16) -> Result<(), Self::Error>;
    fn set_u64(&mut self, key: &str, value: u64) -> Result<(), Self::Error>;
    fn get_str(&self, key: &str) -> Result<Option<String>, Self::Error>;
    fn set_str(&mut self, key: &str, value: &str) -> Result<(), Self::Error>;
    /// Erases `key`; one that isn't there is fine.
    fn erase(&mut self, key: &str) -> Result<(), Self::Error>;
    /// Makes what was set and erased stick.
    fn commit(&mut self) -> Result<(), Self::Error>;
}
