#pragma once

#include "sunsim/core/daylight_program.h"
#include "sunsim/core/device_registry.h"
#include "sunsim/core/light_output.h"
#include "sunsim/core/mode_controller.h"
#include "sunsim/core/status_model.h"
#include "sunsim/ev/bus.h"

namespace sunsim::core {

// All of the pure application logic. Platform code (radio, storage, clock,
// console, display) talks to it only through events on the bus.
struct CoreModules {
    CoreModules(const ev::Post &post, DeviceRegistry::Manifest manifest)
        : mode(post), registry(post, manifest), program(post), output(post), status(post)
    {
    }

    ModeController  mode;
    DeviceRegistry  registry;
    DaylightProgram program;
    LightOutput     output;
    StatusModel     status;
};

// Every edge between core modules, in one place. Platform edges are wired
// separately (main/app.cpp); tests wire only this.
void wireCore(ev::EventBus &bus, CoreModules &m);

} // namespace sunsim::core
