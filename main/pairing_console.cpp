#include "pairing_console.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "freertos/FreeRTOS.h"

#include "button_controller.h"
#include "light_rig.h"
#include "schedule_settings.h"
#include "status.h"
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

constexpr std::string_view kDayNames[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

void printClock(double hours)
{
    const long minutes = std::lround(hours * 60.0);
    std::printf("%02ld:%02ld", minutes / 60, minutes % 60);
}

void printSchedule(const DaylightSchedule::Config &c)
{
    std::printf("hours ");
    printClock(c.active_start_hour);
    std::printf("-");
    printClock(c.active_end_hour);
    std::printf("\nfade  %.0f min\ndays ", c.fade_minutes);
    for (int d = 0; d < 7; ++d) {
        const int day = (d + 1) % 7; // list Monday first
        if ((c.active_days >> day) & 1U) {
            std::printf(" %s", kDayNames[day].data());
        }
    }
    std::printf("\n");
}

// "mon-fri", "all", or a comma list like "mon,wed,sat".
std::optional<uint8_t> parseDays(std::string_view arg)
{
    if (arg == "mon-fri") {
        return DaylightSchedule::kMondayToFriday;
    }
    if (arg == "all") {
        return DaylightSchedule::kEveryDay;
    }
    uint8_t mask = 0;
    while (!arg.empty()) {
        const auto comma = arg.find(',');
        const auto name  = arg.substr(0, comma);
        bool       found = false;
        for (int day = 0; day < 7; ++day) {
            if (kDayNames[day] == name) {
                mask |= 1U << day;
                found = true;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        arg = comma == std::string_view::npos ? std::string_view{} : arg.substr(comma + 1);
    }
    return mask ? std::optional<uint8_t>{mask} : std::nullopt;
}

int cmdConfig(void *ctx, int argc, char **argv)
{
    auto &rig = *static_cast<LightRig *>(ctx);

    ZigbeeLock               lock;
    DaylightSchedule::Config c = rig.scheduleConfig();
    if (argc == 1) {
        printSchedule(c);
        return 0;
    }
    const std::string_view key{argv[1]};
    if (argc != 3) {
        std::printf("usage: config [hours HH:MM-HH:MM | fade <minutes> | days mon-fri|all|mon,tue,...]\n");
        return 1;
    }

    if (key == "hours") {
        int sh = 0, sm = 0, eh = 0, em = 0;
        if (std::sscanf(argv[2], "%d:%d-%d:%d", &sh, &sm, &eh, &em) != 4 || sm < 0 || sm > 59 || em < 0 ||
            em > 59) {
            std::printf("expected HH:MM-HH:MM, e.g. 08:00-19:00\n");
            return 1;
        }
        const double start = sh + sm / 60.0;
        const double end   = eh + em / 60.0;
        if (start < 0.0 || end > 24.0 || start >= end) {
            std::printf("start must be before end, within 00:00-24:00\n");
            return 1;
        }
        c.active_start_hour = start;
        c.active_end_hour   = end;
    } else if (key == "fade") {
        const int minutes = std::atoi(argv[2]);
        if (minutes < 0 || minutes > 180) {
            std::printf("fade must be 0-180 minutes\n");
            return 1;
        }
        c.fade_minutes = minutes;
    } else if (key == "days") {
        const auto days = parseDays(argv[2]);
        if (!days) {
            std::printf("expected mon-fri, all, or a list like mon,tue,wed\n");
            return 1;
        }
        c.active_days = *days;
    } else {
        std::printf("unknown setting '%s' (hours, fade, days)\n", argv[1]);
        return 1;
    }

    rig.setScheduleConfig(c);
    saveScheduleSettings(c);
    printSchedule(c);
    return 0;
}

// Feeds the same ButtonController entry points the real button's Zigbee
// traffic does, so this exercises the whole press -> mode path.
int cmdPress(void *ctx, int argc, char **argv)
{
    auto &button = *static_cast<ButtonController *>(ctx);
    if (argc != 2) {
        std::printf("usage: press single|double|hold\n");
        return 1;
    }
    const std::string_view kind{argv[1]};

    ZigbeeLock lock;
    if (kind == "single") {
        button.onOffOffCommand(true); // resolves to "resume" on the heartbeat after the double-click window
    } else if (kind == "double") {
        button.onSceneRecall();
    } else if (kind == "hold") {
        button.onOffOffCommand(false);
    } else {
        std::printf("usage: press single|double|hold\n");
        return 1;
    }
    return 0;
}

struct StatusContext {
    LightRig         *rig;
    ButtonController *button;
};

void printHoursMinutes(double hours)
{
    const long minutes = std::lround(hours * 60.0);
    std::printf("%ldh %02ldm", minutes / 60, minutes % 60);
}

int cmdStatus(void *ctx, int, char **)
{
    const auto &c = *static_cast<StatusContext *>(ctx);

    StatusSnapshot st;
    {
        ZigbeeLock lock;
        st = takeStatusSnapshot(*c.rig, *c.button);
    }

    static constexpr const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (st.clock_valid) {
        std::printf("time    %s %02d:%02d%s\n", kDays[st.time.tm_wday % 7], st.time.tm_hour, st.time.tm_min,
                    st.simulated ? "  (simulated)" : "");
    } else {
        std::printf("time    NOT SET - waiting for time sync (lights held off)\n");
    }

    std::printf("day     ");
    printHoursMinutes(st.day.day_length_hours);
    std::printf("  (sunrise ");
    printClock(st.day.sunrise_hour);
    std::printf(", sunset ");
    printClock(st.day.sunset_hour);
    std::printf(")\nnext    %s in ", st.day.next_is_sunrise ? "sunrise" : "sunset");
    printHoursMinutes(st.day.hours_until_next);
    std::printf(" (");
    printClock(st.day.next_event_hour);
    std::printf(")\n");

    std::printf("mode    %s", modeName(st.mode));
    if (st.mode == LightingMode::Auto) {
        std::printf(" - %s", st.in_active_window ? "in active hours" : "outside active hours, lights off");
    }
    std::printf("\n");

    const int64_t now_us = esp_timer_get_time();
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const auto &d = st.devices[i];
        std::printf("  %-6s ", roleName(i).data());
        switch (st.links[i]) {
        case DeviceLink::Unpaired:
            std::printf("-  not paired\n");
            continue;
        case DeviceLink::Silent:
            std::printf("o  silent");
            break;
        case DeviceLink::Online:
            std::printf("*  online");
            break;
        }
        if (d.last_heard_us != 0) {
            std::printf(", heard %llds ago", static_cast<long long>((now_us - d.last_heard_us) / 1'000'000));
        }
        if (d.rssi != 0) {
            std::printf(", rssi %d", d.rssi);
        }
        if (!d.has_endpoint) {
            std::printf(", endpoint not found yet");
        }
        std::printf("\n");
    }
    return 0;
}

void printPeaks(const LightRig &rig)
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (static_cast<Role>(i) == Role::Button) {
            continue;
        }
        std::printf("  %-6s %3u%%\n", roleName(i).data(), rig.peakPercent(static_cast<Role>(i)));
    }
}

int cmdIntensity(void *ctx, int argc, char **argv)
{
    const auto &c = *static_cast<StatusContext *>(ctx);
    if (argc == 1) {
        ZigbeeLock lock;
        printPeaks(*c.rig);
        return 0;
    }
    const int percent = argc == 3 ? std::atoi(argv[2]) : 0;
    if (argc != 3 || percent < 1 || percent > 100) {
        std::printf("usage: intensity [<WallA|WallB|WallC|Work|all> <1-100>]\n");
        return 1;
    }

    std::optional<Role> only;
    if (std::string_view{argv[1]} != "all") {
        only = parseRole(argv[1]);
        if (!only || *only == Role::Button) {
            std::printf("unknown light '%s' (WallA, WallB, WallC, Work or all)\n", argv[1]);
            return 1;
        }
    }

    ZigbeeLock lock;
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const auto role = static_cast<Role>(i);
        if (role == Role::Button || (only && *only != role)) {
            continue;
        }
        c.rig->setPeakPercent(role, static_cast<uint8_t>(percent));
        savePeakPercent(role, static_cast<uint8_t>(percent));
        if (c.button->mode() == LightingMode::Auto) {
            c.rig->snapRole(role); // show the change now, not at the next tick
        }
    }
    printPeaks(*c.rig);
    return 0;
}

int cmdRestart(void *, int, char **)
{
    esp_restart();
}

void registerCommand(const char *name, const char *help, const char *hint,
                     esp_console_cmd_func_with_context_t func, void *context)
{
    const esp_console_cmd_t cmd{
        .command        = name,
        .help           = help,
        .hint           = hint,
        .func           = nullptr,
        .argtable       = nullptr,
        .func_w_context = func,
        .context        = context,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
} // namespace

void startPairingConsole(LightRig &rig, ButtonController &button)
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
    registerCommand("roles", "List role assignments", nullptr, &cmdRoles, &rig);
    registerCommand("pair", "Give the next unassigned device to join this role, and open the network",
                    "<WallA|WallB|WallC|Work|Button|cancel>", &cmdPair, &rig);
    registerCommand("unpair", "Forget a role's device (it can then be re-paired)", "<role|all>", &cmdUnpair, &rig);
    registerCommand("open", "Open the network for joining", "[seconds]", &cmdOpen, &rig);
    registerCommand("config", "Show or change the lights' active hours, edge fade and work days",
                    "[hours HH:MM-HH:MM | fade <min> | days mon-fri|all|mon,tue,...]", &cmdConfig, &rig);
    static StatusContext status_ctx{&rig, &button};
    registerCommand("status", "Time, day length, next sunrise/sunset, mode and device links", nullptr, &cmdStatus,
                    &status_ctx);
    registerCommand("intensity", "Show or set each light's peak brightness, 1-100%", "[<light|all> <1-100>]",
                    &cmdIntensity, &status_ctx);
    registerCommand("press", "Simulate the button: single = resume daylight, double = red alert, hold = all off",
                    "single|double|hold", &cmdPress, &button);
    registerCommand("wifi", "Show, set or clear the Wi-Fi network used for the boot-time clock sync",
                    "[<ssid> [password]] | clear", &cmdWifi, &rig);
    registerCommand("restart", "Reboot (keeps roles, network and Wi-Fi settings)", nullptr, &cmdRestart, &rig);
    registerCommand("factory_reset", "Forget all roles AND the Zigbee network, then restart", nullptr,
                    &cmdFactoryReset, &rig);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

} // namespace sunsim
