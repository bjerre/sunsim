// sunsim::sim - sunsim on simulated hardware, as one C++ module: virtual
// time (:scheduler), the board's clock on it (:sim_clock), and a Zigbee
// network of virtual bulbs and buttons (:sim_network), which stand in for
// the platform modules. World puts them together with the core, as
// main/main.cpp does with the board's. Pure C++, like the core.
export module sunsim.sim;

import std.compat;
import sunsim.ev;
import sunsim.core;

export import :scheduler;
export import :sim_clock;
export import :sim_network;

export namespace sunsim::sim {

// The core, wired as the firmware wires it, with SimClock and SimNetwork
// for the board's clock and radio. Add the devices (network.add()), then
// start() and run (scheduler.runFor()).
struct World {
    World(ev::TimeReading start, ev::Addresses addresses = {}, std::vector<core::LightGroup> groups = {});

    World(const World &)            = delete;
    World &operator=(const World &) = delete;

    void start();
    void runFor(ev::BootClock::duration duration) { scheduler.runFor(duration); }
    [[nodiscard]] ev::Instant now() const noexcept { return scheduler.now(); }

    ev::EventBus      bus;
    Scheduler         scheduler{bus};
    core::CoreModules core;
    SimClock          clock;
    SimNetwork        network;
};

} // namespace sunsim::sim

namespace sunsim::sim {

World::World(ev::TimeReading start, ev::Addresses addresses, std::vector<core::LightGroup> groups)
    : core(bus.post(), addresses, ev::kDefaultInstallation, std::move(groups)), clock(bus.post(), scheduler, start),
      network(bus.post(), scheduler)
{
    core::wireCore(bus, core);
    bus.attach(clock, network);
}

void World::start()
{
    network.start();
    clock.start();
    scheduler.runFor({}); // what starting sets off
}

} // namespace sunsim::sim
