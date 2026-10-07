#pragma once

#include "sunsim/core/core.h"
#include "sunsim/ev/bus.h"
#include "sunsim/platform/clock.h"
#include "sunsim/platform/settings_store.h"
#include "sunsim/platform/status_slot.h"
#include "sunsim/platform/zigbee_adapter.h"

namespace sunsim {

// Owns the event bus and every module, wires them, and runs the app task:
// the single consumer of the bus. See EVENT_DESIGN.md.
class App {
public:
    App();

    App(const App &)            = delete;
    App &operator=(const App &) = delete;

    // Loads settings, syncs time, starts the radio, the app task, the
    // display and the console. Returns once everything is running.
    void run();

private:
    void wirePlatform();
    static void appTask(void *arg);

    ev::EventBus            bus_;
    platform::SettingsStore settings_;
    platform::Clock         clock_;
    platform::ZigbeeAdapter zigbee_;
    platform::StatusSlot    status_;
    core::CoreModules       core_;
};

} // namespace sunsim
