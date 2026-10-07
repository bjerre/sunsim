#include "app.h"

#include <cstdlib>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "role_manifest.h"
#include "sunsim/platform/console.h"
#include "sunsim/platform/status_display.h"

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
} // namespace

App::App()
    : settings_(bus_.post(), defaultProgram()), clock_(bus_.post()), zigbee_(bus_.post()),
      core_(bus_.post(), kRoleManifest)
{
    core::wireCore(bus_, core_);
    wirePlatform();
}

// Edges between the platform and the rest; core::wireCore has the others.
void App::wirePlatform()
{
    using namespace ev;

    // Clock
    bus_.on<Heartbeat>([this](const Heartbeat &) { clock_.onHeartbeat(); });
    bus_.on<ConfigChanged>([this](const ConfigChanged &e) { clock_.onConfig(e); });

    // Settings
    bus_.on<SetConfig>([this](const SetConfig &e) { settings_.onSetConfig(e); });
    bus_.on<SetIntensity>([this](const SetIntensity &e) { settings_.onSetIntensity(e); });
    bus_.on<PersistRoles>([this](const PersistRoles &e) { settings_.onPersistRoles(e); });

    // Radio
    bus_.on<SetLight>([this](const SetLight &e) { zigbee_.onSetLight(e); });
    bus_.on<Discover>([this](const Discover &e) { zigbee_.onDiscover(e); });
    bus_.on<Bind>([this](const Bind &e) { zigbee_.onBind(e); });
    bus_.on<Ping>([this](const Ping &e) { zigbee_.onPing(e); });
    bus_.on<OpenNetwork>([this](const OpenNetwork &e) { zigbee_.onOpenNetwork(e); });
    bus_.on<RoleAssigned>([this](const RoleAssigned &e) { zigbee_.onRoleAssigned(e); });
    bus_.on<RoleCleared>([this](const RoleCleared &e) { zigbee_.onRoleCleared(e); });

    // Erase the stored roles first: the radio's reset reboots and never returns.
    bus_.on<FactoryReset>([this](const FactoryReset &) {
        settings_.onFactoryReset();
        zigbee_.onFactoryReset();
    });

    // Status, for the console and display tasks
    bus_.on<StatusChanged>([this](const StatusChanged &e) { status_.set(*e.snapshot); });
}

void App::run()
{
    settings_.loadAll(); // queued until the app task starts processing

    clock_.startTimeSync(); // before the radio starts, so Wi-Fi has it to itself at boot
    clock_.start();
    zigbee_.start();

    // 8 KB: listeners post further events, write NVS and log while processing.
    xTaskCreate(&App::appTask, "app", 8192, this, 5, nullptr);

    platform::startStatusDisplay(status_);
    platform::startConsole(bus_.post(), status_);
}

void App::appTask(void *arg)
{
    auto &app = *static_cast<App *>(arg);

    // Handlers may call into the Zigbee stack, which needs its lock - and
    // the lock only exists once the stack is initialised.
    app.zigbee_.waitUntilReady();
    ESP_LOGI(kTag, "Processing events");

    for (;;) {
        app.bus_.wait();
        platform::ZigbeeAdapter::lock();
        app.bus_.process();
        platform::ZigbeeAdapter::unlock();
    }
}

} // namespace sunsim
