module;

#include "sunsim/ev/bus.h"

// sunsim::core - all of the pure application logic, as one module. Each
// partition is one module (or the math it uses); importing sunsim.core
// gets them all.
export module sunsim.core;

export import :day_info;
export import :daylight;
export import :daylight_program;
export import :device_registry;
export import :light_output;
export import :mode_controller;
export import :off_program;
export import :program;
export import :program_host;
export import :red_alert_program;
export import :solar;
export import :status_model;

export namespace sunsim::core {

// All of the pure application logic. Platform code (radio, storage, clock,
// console, display) talks to it only through events on the bus.
struct CoreModules {
    CoreModules(const ev::Post &post, DeviceRegistry::Manifest manifest)
        : mode(post), registry(post, manifest), programs(post), output(post), status(post)
    {
    }

    ModeController  mode;
    DeviceRegistry  registry;
    ProgramHost     programs;
    LightOutput     output;
    StatusModel     status;
};

// Every edge between core modules, in one place. Platform edges are wired
// separately (main/app.cpp); tests wire only this.
void wireCore(ev::EventBus &bus, CoreModules &m);

} // namespace sunsim::core
