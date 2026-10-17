// sunsim::core - all of the pure application logic, as one C++ module: a
// partition per class (and for the math), all re-exported here.
export module sunsim.core;

import std.compat;
import sunsim.ev;

export import :day_info;
export import :daylight;
export import :daylight_program;
export import :device_registry;
export import :light_output;
export import :mode_controller;
export import :off_program;
export import :overlays;
export import :program;
export import :program_host;
export import :red_alert_program;
export import :solar;
export import :sky;
export import :status_model;
export import :tuning_program;

export namespace sunsim::core {

// All of the pure application logic. Platform code (radio, storage, clock,
// console, display) talks to it only through events on the bus.
struct CoreModules {
    CoreModules(const ev::Post &post, DeviceRegistry::Manifest manifest)
        : mode(post), registry(post, manifest), programs(post), overlays(post), output(post), status(post)
    {
    }

    ModeController  mode;
    DeviceRegistry  registry;
    ProgramHost     programs;
    Overlays        overlays;
    LightOutput     output;
    StatusModel     status;
};

// Subscribes every core module to the events it handles (its on()
// overloads). Platform modules are attached separately (main/main.cpp);
// tests attach only these.
void wireCore(ev::EventBus &bus, CoreModules &m);

} // namespace sunsim::core

namespace sunsim::core {

void wireCore(ev::EventBus &bus, CoreModules &m)
{
    bus.attach(m.mode, m.registry, m.programs, m.overlays, m.output, m.status);
}

} // namespace sunsim::core
