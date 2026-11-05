//! Reading the weather services' replies (Open-Meteo, NWS) as they stream
//! in: pure, so the host tests cover it.

pub mod json;
pub mod readers;

pub use json::{JsonScanner, JsonSink, Scalar};
pub use readers::*;
