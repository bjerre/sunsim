// The Zigbee network, as the rest of the app sees it: only events. The
// board's radio is platform::ZigbeeAdapter (esp-zigbee-lib); a simulated
// one is sim::SimNetwork. Either is attached to the bus like any module,
// and the core can't tell them apart.

export module sunsim.ev:radio;

import std.compat;
import :bus;
import :events;

export namespace sunsim::ev {

// A radio: it takes these requests, each with `void on(const T &)`.
//
//   SendsWaiting  -> SendSlot, soon: room for the next bulb command
//   SendLight     -> the command sent; AckReceived when a bulb answers one
//                    sent to it alone (not one sent to a group)
//   Discover      -> EndpointFound (endpoint 0: none, or no answer)
//   Bind          -> BindResult
//   Ping          -> PingResult
//   OpenNetwork   -> devices that join meanwhile: DeviceAnnounced
//   FactoryReset  -> the network cleared and formed again: NetworkUp
//
// And it posts NetworkUp when the network is up, and ButtonPressed for a
// bound button's presses. (What a radio posts, the compiler can't check.)
template <class M>
concept Radio = HandlerOf<M, SendsWaiting> && HandlerOf<M, SendLight> && HandlerOf<M, Discover> &&
                HandlerOf<M, Bind> && HandlerOf<M, Ping> && HandlerOf<M, OpenNetwork> &&
                HandlerOf<M, FactoryReset>;

} // namespace sunsim::ev
