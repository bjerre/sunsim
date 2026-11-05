//! The platform's Storage on NVS: namespace "sunsim" in the default
//! partition, as the C++ firmware keeps it.

use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use esp_idf_svc::sys::EspError;
use platform::Storage;

const NAMESPACE: &str = "sunsim";

pub struct Nvs(EspNvs<NvsDefault>);

impl Nvs {
    pub fn open(partition: EspDefaultNvsPartition) -> Result<Self, EspError> {
        Ok(Nvs(EspNvs::new(partition, NAMESPACE, true)?))
    }
}

impl Storage for Nvs {
    type Error = EspError;

    fn get_u8(&self, key: &str) -> Result<Option<u8>, EspError> {
        self.0.get_u8(key)
    }
    fn get_u16(&self, key: &str) -> Result<Option<u16>, EspError> {
        self.0.get_u16(key)
    }
    fn get_u64(&self, key: &str) -> Result<Option<u64>, EspError> {
        self.0.get_u64(key)
    }
    fn set_u8(&mut self, key: &str, value: u8) -> Result<(), EspError> {
        self.0.set_u8(key, value)
    }
    fn set_u16(&mut self, key: &str, value: u16) -> Result<(), EspError> {
        self.0.set_u16(key, value)
    }
    fn set_u64(&mut self, key: &str, value: u64) -> Result<(), EspError> {
        self.0.set_u64(key, value)
    }
    fn get_str(&self, key: &str) -> Result<Option<String>, EspError> {
        let mut buf = [0u8; 65]; // a Wi-Fi password, the longest, and its terminator
        Ok(self.0.get_str(key, &mut buf)?.map(str::to_string))
    }
    fn set_str(&mut self, key: &str, value: &str) -> Result<(), EspError> {
        self.0.set_str(key, value)
    }
    fn erase(&mut self, key: &str) -> Result<(), EspError> {
        self.0.remove(key).map(|_| ())
    }
    fn commit(&mut self) -> Result<(), EspError> {
        Ok(()) // EspNvs commits each set and erase as it makes it
    }
}
