// app_main: builds the App - the event bus, every module, and their wiring -
// and runs it. See ARCHITECTURE.md.

#include <stdlib.h> // setenv (POSIX)
#include <time.h>   // tzset (POSIX)

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

// Every #include before the first import (GCC).
import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.platform;

namespace sunsim {

namespace {

constexpr const char *kTag = "APP";

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

    platform::InstallationProfile installation_;
    core::CoreRuntime       runtime_{ev::kAddressOf, installation_.installation()};
    platform::SettingsStore settings_;
    platform::Clock         clock_;
    platform::InternetSync  internet_;
    platform::ZigbeeAdapter zigbee_;
    platform::StatusSlot    status_;
    platform::Console       console_{runtime_.post(), status_, installation_};
};

} // namespace

App::App()
    : settings_(runtime_.post(), defaultProgram(), installation_.installation().topology.peak_of),
      clock_(runtime_.post()),
      internet_(runtime_.post(), defaultProgram().latitude_deg, defaultProgram().longitude_deg), zigbee_(runtime_.post())
{
    // The platform modules, each to the events it handles. Settings before
    // the radio: on FactoryReset the stored roles are erased first, as the
    // radio's reset reboots and never returns.
    runtime_.bus().attach(settings_, clock_, internet_, zigbee_, status_);
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
    // the lock only exists once the stack is initialized.
    app.zigbee_.waitUntilReady();
    ev::log::info(kTag, "Processing events");

    platform::ZigbeeAdapter::StackLock stack;
    for (;;) {
        app.runtime_.bus().wait();
        const std::scoped_lock lock{stack};
        (void)app.runtime_.process(); // wait() guarantees this pass has work; no drain needed
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
