// sunsim::core - all of the pure application logic, as one C++ module: a
// partition per class (and for the math), all re-exported here.
export module sunsim.core;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export import :day_info;
export import :daylight;
export import :daylight_program;
export import :device_registry;
export import :gate;
export import :light_output;
export import :mode_controller;
export import :off_program;
export import :overlays;
export import :program;
export import :program_host;
export import :red_alert_program;
export import :solar;
export import :sky;
export import :sky_model;
export import :status_model;
export import :tuning_program;

export namespace sunsim::core {

// All of the pure application logic. Platform code (radio, storage, clock,
// console, display) talks to it only through events on the bus.
struct CoreModules {
    CoreModules(const ev::Post &post, ev::Addresses addresses,
                ev::Installation profile = ev::kDefaultInstallation)
        : installation(std::move(profile)), mode(post, installation), registry(post, addresses), sky(post),
          programs(post, installation), overlays(post, installation), output(post), status(post, installation)
    {
    }

    ev::Installation installation;
    ModeController  mode;
    DeviceRegistry  registry;
    SkyModel        sky;
    ProgramHost     programs;
    Overlays        overlays;
    LightOutput     output;
    StatusModel     status;
};

// Subscribes every core module to the events it handles (its on()
// overloads). Platform modules are attached separately (main/main.cpp);
// tests attach only these.
void wireCore(ev::EventBus &bus, CoreModules &m);

// The portable application runtime: owns the bus and wired core, while
// platform adapters attach separately. A browser scheduler can call process()
// whenever it has work; the firmware may wait on bus() before doing so.
class CoreRuntime {
public:
    explicit CoreRuntime(ev::Addresses addresses, ev::Installation profile = ev::kDefaultInstallation)
        : core_(bus_.post(), addresses, std::move(profile))
    {
        wireCore(bus_, core_);
    }

    CoreRuntime(const CoreRuntime &)            = delete;
    CoreRuntime &operator=(const CoreRuntime &) = delete;

    [[nodiscard]] ev::EventBus &bus() noexcept { return bus_; }
    [[nodiscard]] const ev::Post &post() const noexcept { return bus_.post(); }
    [[nodiscard]] bool process() { return bus_.process(); }
    void drain() { bus_.drain(); }

private:
    ev::EventBus bus_;
    CoreModules  core_;
};

} // namespace sunsim::core

namespace sunsim::core {

void wireCore(ev::EventBus &bus, CoreModules &m)
{
    bus.attach(m.mode, m.registry, m.sky, m.programs, m.overlays, m.output, m.status);
}

} // namespace sunsim::core
