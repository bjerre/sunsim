// app_main: builds the App - the event bus, every module, and their wiring -
// and runs it. See ARCHITECTURE.md.

#include <stdlib.h> // setenv (POSIX)
#include <time.h>   // tzset (POSIX)

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "role_manifest.h"

// Every #include before the first import (GCC).
import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.platform;

namespace sunsim {

namespace {

constexpr const char *kTag = "APP";

// role_manifest.json, checked against the role layout at compile time.
constexpr core::DeviceRegistry::Manifest kManifest = core::DeviceRegistry::manifestOf(kRoleManifest);

ev::ProgramConfig defaultProgram()
{
    ev::ProgramConfig c;
    c.latitude_deg  = std::strtod(CONFIG_SUNSIM_LATITUDE_DEG, nullptr);
    c.longitude_deg = std::strtod(CONFIG_SUNSIM_LONGITUDE_DEG, nullptr);
    return c;
}

// Owns the event bus and every module, attaches them, and runs the app task:
// the single consumer of the bus. See ARCHITECTURE.md.
class App {
public:
    App();

    App(const App &)            = delete;
    App &operator=(const App &) = delete;

    // Loads settings, syncs time, starts the radio, the app task, the
    // display and the console. Returns once everything is running.
    void run();

private:
    static void appTask(void *arg);

    ev::EventBus            bus_;
    platform::SettingsStore settings_;
    platform::Clock         clock_;
    platform::InternetSync  internet_;
    platform::ZigbeeAdapter zigbee_;
    platform::StatusSlot    status_;
    platform::Console       console_{bus_.post(), status_};
    core::CoreModules       core_;
};

} // namespace

App::App()
    : settings_(bus_.post(), defaultProgram()), clock_(bus_.post()),
      internet_(bus_.post(), defaultProgram().latitude_deg, defaultProgram().longitude_deg), zigbee_(bus_.post()),
      core_(bus_.post(), kManifest)
{
    core::wireCore(bus_, core_);
    // The platform modules, each to the events it handles. Settings before
    // the radio: on FactoryReset the stored roles are erased first, as the
    // radio's reset reboots and never returns.
    bus_.attach(settings_, clock_, internet_, zigbee_, status_);
}

void App::run()
{
    settings_.loadAll(); // queued until the app task starts processing

    internet_.start(); // waits for the boot sync: before the radio starts, so Wi-Fi has it to itself
    clock_.start();
    zigbee_.start();

    // 8 KB: listeners post further events, write NVS and log while processing.
    xTaskCreate(&App::appTask, "app", 8192, this, 5, nullptr);

    platform::startStatusDisplay(status_);
    console_.start();
}

void App::appTask(void *arg)
{
    auto &app = *static_cast<App *>(arg);

    // Handlers may call into the Zigbee stack, which needs its lock - and
    // the lock only exists once the stack is initialised.
    app.zigbee_.waitUntilReady();
    ESP_LOGI(kTag, "Processing events");

    platform::ZigbeeAdapter::StackLock stack;
    for (;;) {
        app.bus_.wait();
        const std::scoped_lock lock{stack};
        app.bus_.process();
    }
}

} // namespace sunsim

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    setenv("TZ", CONFIG_SUNSIM_TZ, 1);
    tzset();

    static sunsim::App app;
    app.run();
}
