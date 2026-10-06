#include "pairing_console.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_zigbee.h"
#include "freertos/FreeRTOS.h"

#include "light_rig.h"
#include "wifi_time_sync.h"

namespace sunsim {

namespace {
constexpr const char *kTag = "PAIRING";

constexpr uint8_t kDefaultOpenSeconds = 180;

// Console commands run on the REPL task, outside any Zigbee callback, so
// every touch of LightRig or the stack holds the Zigbee lock (LightRig's own
// state is otherwise only mutated from Zigbee callbacks, under that lock).
class ZigbeeLock {
public:
    ZigbeeLock() { esp_zigbee_lock_acquire(portMAX_DELAY); }
    ~ZigbeeLock() { esp_zigbee_lock_release(); }
    ZigbeeLock(const ZigbeeLock &)            = delete;
    ZigbeeLock &operator=(const ZigbeeLock &) = delete;
};

void printRoleNames()
{
    std::printf("roles:");
    for (const auto name : kRoleNames) {
        std::printf(" %.*s", static_cast<int>(name.size()), name.data());
    }
    std::printf("\n");
}

void openNetwork(uint8_t seconds)
{
    const ezb_err_t err = ezb_bdb_open_network(seconds);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(kTag, "Could not open network (err 0x%x) - has it finished forming?", err);
    }
}

int cmdRoles(void *ctx, int, char **)
{
    auto      &rig = *static_cast<LightRig *>(ctx);
    ZigbeeLock lock;
    rig.logRoles();
    return 0;
}

int cmdPair(void *ctx, int argc, char **argv)
{
    auto &rig = *static_cast<LightRig *>(ctx);
    if (argc != 2) {
        std::printf("usage: pair <role>|cancel\n");
        printRoleNames();
        return 1;
    }
    const std::string_view arg{argv[1]};

    ZigbeeLock lock;
    if (arg == "cancel") {
        rig.cancelPairing();
        ESP_LOGI(kTag, "Pairing cancelled");
        return 0;
    }
    const auto role = parseRole(arg);
    if (!role) {
        std::printf("unknown role '%s'\n", argv[1]);
        printRoleNames();
        return 1;
    }
    if (rig.isAssigned(*role)) {
        ESP_LOGW(kTag, "%s is already assigned; the next device to join will replace it", argv[1]);
    }
    rig.armPairing(*role);
    openNetwork(kDefaultOpenSeconds);
    ESP_LOGI(kTag, "Armed %s: factory-reset / power on the device now. The next device that joins and "
                   "doesn't already hold a role becomes %s.",
             argv[1], argv[1]);
    return 0;
}

int cmdUnpair(void *ctx, int argc, char **argv)
{
    auto &rig = *static_cast<LightRig *>(ctx);
    if (argc != 2) {
        std::printf("usage: unpair <role>|all\n");
        printRoleNames();
        return 1;
    }
    const std::string_view arg{argv[1]};

    ZigbeeLock lock;
    if (arg == "all") {
        rig.unpairAll();
        ESP_LOGI(kTag, "All roles cleared");
        return 0;
    }
    const auto role = parseRole(arg);
    if (!role) {
        std::printf("unknown role '%s'\n", argv[1]);
        printRoleNames();
        return 1;
    }
    rig.unpair(*role);
    ESP_LOGI(kTag, "%s cleared", argv[1]);
    return 0;
}

int cmdOpen(void *, int argc, char **argv)
{
    int seconds = kDefaultOpenSeconds;
    if (argc == 2) {
        seconds = std::atoi(argv[1]);
    }
    if (seconds < 1 || seconds > 254) {
        std::printf("seconds must be 1-254\n");
        return 1;
    }
    ZigbeeLock lock;
    openNetwork(static_cast<uint8_t>(seconds));
    return 0;
}

int cmdFactoryReset(void *ctx, int, char **)
{
    auto &rig = *static_cast<LightRig *>(ctx);
    ESP_LOGW(kTag, "Clearing all roles and the Zigbee network, then restarting");
    esp_zigbee_lock_acquire(portMAX_DELAY);
    rig.unpairAll();
    esp_zigbee_factory_reset(); // does not return
}

int cmdWifi(void *, int argc, char **argv)
{
    if (argc == 1) {
        const WifiCredentials creds = loadWifiCredentials();
        if (creds.ssid.empty()) {
            std::printf("no Wi-Fi configured; set one with: wifi <ssid> [password]\n");
        } else {
            std::printf("ssid \"%s\" (%s, %s)\n", creds.ssid.c_str(),
                        creds.from_nvs ? "set from console" : "menuconfig fallback",
                        creds.password.empty() ? "open network" : "password set");
        }
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "clear") {
        clearWifiCredentials();
        std::printf("cleared; falling back to menuconfig Wi-Fi settings on next boot\n");
        return 0;
    }
    if (argc > 3) {
        std::printf("usage: wifi [<ssid> [password]] | wifi clear   (quote an SSID with spaces: \"My Net\")\n");
        return 1;
    }
    if (!saveWifiCredentials(argv[1], argc == 3 ? argv[2] : "")) {
        std::printf("not saved: SSID must be 1-32 chars, password at most 64\n");
        return 1;
    }
    std::printf("saved \"%s\"; type `restart` to sync time with it\n", argv[1]);
    return 0;
}

int cmdRestart(void *, int, char **)
{
    esp_restart();
}

void registerCommand(const char *name, const char *help, const char *hint,
                     esp_console_cmd_func_with_context_t func, LightRig &rig)
{
    const esp_console_cmd_t cmd{
        .command        = name,
        .help           = help,
        .hint           = hint,
        .func           = nullptr,
        .argtable       = nullptr,
        .func_w_context = func,
        .context        = &rig,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
} // namespace

void startPairingConsole(LightRig &rig)
{
    esp_console_repl_t       *repl        = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt                    = "sunsim>";

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl));
#else
#error "pairing console needs a UART or USB-Serial-JTAG primary console"
#endif

    ESP_ERROR_CHECK(esp_console_register_help_command());
    registerCommand("roles", "List role assignments", nullptr, &cmdRoles, rig);
    registerCommand("pair", "Give the next unassigned device to join this role, and open the network",
                    "<WallA|WallB|WallC|Work|Button|cancel>", &cmdPair, rig);
    registerCommand("unpair", "Forget a role's device (it can then be re-paired)", "<role|all>", &cmdUnpair, rig);
    registerCommand("open", "Open the network for joining", "[seconds]", &cmdOpen, rig);
    registerCommand("wifi", "Show, set or clear the Wi-Fi network used for the boot-time clock sync",
                    "[<ssid> [password]] | clear", &cmdWifi, rig);
    registerCommand("restart", "Reboot (keeps roles, network and Wi-Fi settings)", nullptr, &cmdRestart, rig);
    registerCommand("factory_reset", "Forget all roles AND the Zigbee network, then restart", nullptr,
                    &cmdFactoryReset, rig);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

} // namespace sunsim
