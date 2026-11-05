//! The Zigbee network, as the rest of the app sees it: only events. The
//! board's radio is the platform's ZigbeeAdapter (esp-zigbee-lib); a
//! simulated one is sim::SimNetwork. Either is attached to the bus like any
//! module, and the core can't tell them apart.

use crate::events::*;

/// A radio: it takes these requests.
///
/// ```text
/// SendsWaiting  -> SendSlot, soon: room for the next bulb command
/// SendLight     -> the command sent; AckReceived when a bulb answers one
///                  sent to it alone (not one sent to a group)
/// Discover      -> EndpointFound (endpoint 0: none, or no answer)
/// Bind          -> BindResult
/// Ping          -> PingResult
/// OpenNetwork   -> devices that join meanwhile: DeviceAnnounced
/// FactoryReset  -> the network cleared and formed again: NetworkUp
/// ```
///
/// And it posts NetworkUp when the network is up, and ButtonPressed for a
/// bound button's presses. (What a radio posts, the compiler can't check.)
/// Its Module::on passes the requests to `requests()`, which hands each to
/// its method here, and handles anything else it wants itself.
pub trait Radio {
    fn sends_waiting(&mut self, e: &SendsWaiting);
    fn send_light(&mut self, e: &SendLight);
    fn discover(&mut self, e: &Discover);
    fn bind(&mut self, e: &Bind);
    fn ping(&mut self, e: &Ping);
    fn open_network(&mut self, e: &OpenNetwork);
    fn factory_reset(&mut self, e: &FactoryReset);
}

/// `e` to the radio's method for it; false if it isn't a request.
pub fn requests(radio: &mut impl Radio, e: &Event) -> bool {
    match e {
        Event::SendsWaiting(e) => radio.sends_waiting(e),
        Event::SendLight(e) => radio.send_light(e),
        Event::Discover(e) => radio.discover(e),
        Event::Bind(e) => radio.bind(e),
        Event::Ping(e) => radio.ping(e),
        Event::OpenNetwork(e) => radio.open_network(e),
        Event::FactoryReset(e) => radio.factory_reset(e),
        _ => return false,
    }
    true
}
