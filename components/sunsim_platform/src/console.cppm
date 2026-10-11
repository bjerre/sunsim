module;

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"

export module sunsim.platform:console;

import std.compat;
import sunsim.ev;
import :status_slot;
import :format;
import :wifi_time_sync;

export namespace sunsim::platform {

// Serial (idf.py monitor) REPL. Commands post requests as events and read
// state from the latest StatusSnapshot; they never touch app state directly.
//
//   status                time, day length, next sunrise/sunset, mode, device links
//   roles                 list role assignments
//   pair <role>           next unassigned device to join gets <role>; opens the network
//   pair cancel           disarm a pending `pair`
//   unpair <role>|all     forget role assignment(s)
//   open [seconds]        open the network for joining (default 180)
//   config                show active/work hours, fade, sun shift, amber, work days
//   config <key> <value>  change one (see `help config`)
//   intensity [<light|all> <1-100>]   per-light peak brightness, saved
//   press single|double|hold          simulate the button
//   wifi                  list saved Wi-Fi networks (the strongest in range is used)
//   wifi add <ssid> [pw]  add a network (up to 5), or change its password
//   wifi remove <ssid>    forget one; `wifi clear` forgets all
//   restart               reboot, keeping everything
//   factory_reset         forget all roles AND the Zigbee network, then restart
//
// <role> is one of WallA, WallB, WallC, Work, Button.
void startConsole(const ev::Post &post, const StatusSlot &status);

} // namespace sunsim::platform

namespace sunsim::platform {

namespace {
// esp_console callbacks are plain functions; this is what they reach.
struct Context {
    ev::Post          post;
    const StatusSlot *status = nullptr;
};
Context g_ctx;

std::optional<ev::StatusSnapshot> snapshot()
{
    auto s = g_ctx.status->get();
    if (!s) {
        std::printf("still starting up; try again in a moment\n");
    }
    return s;
}

void printRoleNames()
{
    std::printf("roles:");
    for (const auto name : ev::kRoleNames) {
        std::printf(" %.*s", static_cast<int>(name.size()), name.data());
    }
    std::printf("\n");
}

// The role named by `arg`, or nullopt after saying why not.
std::optional<ev::Role> parseRoleArg(const char *arg)
{
    const auto role = ev::parseRole(arg);
    if (!role) {
        std::printf("unknown role '%s'\n", arg);
        printRoleNames();
    }
    return role;
}

void printClock(double hours)
{
    char text[16];
    formatClock(text, sizeof(text), hours);
    std::fputs(text, stdout);
}

void printHoursMinutes(double hours)
{
    char text[16];
    formatDuration(text, sizeof(text), hours);
    std::fputs(text, stdout);
}

// ---- roles / pair / unpair / open ---------------------------------------------

int cmdRoles(int, char **)
{
    const auto st = snapshot();
    if (!st) {
        return 1;
    }
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        const ev::DeviceRecord &d = st->records[i];
        if (d.valid) {
            std::printf("%-6s ext=0x%016llx short=0x%04x ep=%d\n", ev::roleName(i).data(),
                        static_cast<unsigned long long>(d.ext_addr), d.short_addr, d.endpoint);
        } else {
            std::printf("%-6s (unassigned)\n", ev::roleName(i).data());
        }
    }
    if (st->armed) {
        std::printf("Armed: next joining device becomes %s\n", ev::roleName(*st->armed).data());
    }
    return 0;
}

int cmdPair(int argc, char **argv)
{
    if (argc != 2) {
        std::printf("usage: pair <role>|cancel\n");
        printRoleNames();
        return 1;
    }
    if (std::string_view{argv[1]} == "cancel") {
        g_ctx.post(ev::PairCancelled{});
        return 0;
    }
    const auto role = parseRoleArg(argv[1]);
    if (!role) {
        return 1;
    }
    g_ctx.post(ev::PairRequested{*role});
    return 0;
}

int cmdUnpair(int argc, char **argv)
{
    if (argc != 2) {
        std::printf("usage: unpair <role>|all\n");
        printRoleNames();
        return 1;
    }
    if (std::string_view{argv[1]} == "all") {
        g_ctx.post(ev::UnpairRequested{});
        return 0;
    }
    const auto role = parseRoleArg(argv[1]);
    if (!role) {
        return 1;
    }
    g_ctx.post(ev::UnpairRequested{*role});
    return 0;
}

int cmdOpen(int argc, char **argv)
{
    const int seconds = argc == 2 ? std::atoi(argv[1]) : ev::kDefaultOpenTime.count();
    if (seconds < 1 || seconds > 254) {
        std::printf("seconds must be 1-254\n");
        return 1;
    }
    g_ctx.post(ev::OpenNetwork{ev::OpenTime{seconds}});
    return 0;
}

// ---- config ---------------------------------------------------------------------

constexpr std::string_view kDayNames[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

void printSchedule(const ev::ProgramConfig &c)
{
    std::printf("hours ");
    printClock(c.active_start_hour);
    std::printf("-");
    printClock(c.active_end_hour);
    std::printf("\nfade  %.0f min\nwork  ", c.fade_minutes);
    printClock(c.work_start_hour);
    std::printf("-");
    printClock(c.work_end_hour);
    std::printf("  (work light cool -> warm)\nsun   %u%% (wall dimming away from the sun)\n"
                "amber %s (sunrise/sunset glow below %.0f deg sun elevation)\ndays ",
                c.sun_shift_percent, c.amber_enabled ? "on" : "off", c.golden_elevation_deg);
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
        return ev::kMondayToFriday;
    }
    if (arg == "all") {
        return ev::kEveryDay;
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

int cmdConfig(int argc, char **argv)
{
    const auto st = snapshot();
    if (!st) {
        return 1;
    }
    ev::ProgramConfig c = st->config;
    if (argc == 1) {
        printSchedule(c);
        return 0;
    }
    const std::string_view key{argv[1]};
    if (argc != 3) {
        std::printf("usage: config [hours HH:MM-HH:MM | workhours HH:MM-HH:MM | fade <minutes> |\n"
                    "               sunshift <0-100> | amber on|off | days mon-fri|all|mon,tue,...]\n");
        return 1;
    }

    if (key == "hours" || key == "workhours") {
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
        if (key == "hours") {
            c.active_start_hour = start;
            c.active_end_hour   = end;
        } else {
            c.work_start_hour = start;
            c.work_end_hour   = end;
        }
    } else if (key == "fade") {
        const int minutes = std::atoi(argv[2]);
        if (minutes < 0 || minutes > 180) {
            std::printf("fade must be 0-180 minutes\n");
            return 1;
        }
        c.fade_minutes = minutes;
    } else if (key == "sunshift") {
        const int percent = std::atoi(argv[2]);
        if (percent < 0 || percent > 100 || (percent == 0 && argv[2][0] != '0')) {
            std::printf("sunshift must be 0-100 (%%; 0 = off)\n");
            return 1;
        }
        c.sun_shift_percent = static_cast<uint8_t>(percent);
    } else if (key == "amber") {
        const std::string_view value{argv[2]};
        if (value != "on" && value != "off") {
            std::printf("amber must be on or off\n");
            return 1;
        }
        c.amber_enabled = value == "on";
    } else if (key == "days") {
        const auto days = parseDays(argv[2]);
        if (!days) {
            std::printf("expected mon-fri, all, or a list like mon,tue,wed\n");
            return 1;
        }
        c.active_days = *days;
    } else {
        std::printf("unknown setting '%s' (hours, workhours, fade, sunshift, amber, days)\n", argv[1]);
        return 1;
    }

    g_ctx.post(ev::SetConfig{c});
    printSchedule(c);
    return 0;
}

// ---- intensity --------------------------------------------------------------------

void printPeaks(const ev::Peaks &peaks)
{
    for (std::size_t i = 0; i < ev::kBulbCount; ++i) {
        std::printf("  %-6s %3u%%\n", ev::roleName(i).data(), peaks[i]);
    }
}

int cmdIntensity(int argc, char **argv)
{
    const auto st = snapshot();
    if (!st) {
        return 1;
    }
    if (argc == 1) {
        printPeaks(st->peaks);
        return 0;
    }
    const int percent = argc == 3 ? std::atoi(argv[2]) : 0;
    if (argc != 3 || percent < 1 || percent > 100) {
        std::printf("usage: intensity [<WallA|WallB|WallC|Work|all> <1-100>]\n");
        return 1;
    }
    std::optional<ev::Role> only;
    if (std::string_view{argv[1]} != "all") {
        only = ev::parseRole(argv[1]);
        if (!only || !ev::isBulb(*only)) {
            std::printf("unknown light '%s' (WallA, WallB, WallC, Work or all)\n", argv[1]);
            return 1;
        }
    }

    g_ctx.post(ev::SetIntensity{only, static_cast<uint8_t>(percent)});
    ev::Peaks peaks = st->peaks;
    for (std::size_t i = 0; i < ev::kBulbCount; ++i) {
        if (!only || ev::indexOf(*only) == i) {
            peaks[i] = static_cast<uint8_t>(percent);
        }
    }
    printPeaks(peaks);
    return 0;
}

// ---- press / status ---------------------------------------------------------------

// Posts the same event the real button does, so this exercises the whole
// press -> mode -> lights path.
int cmdPress(int argc, char **argv)
{
    const std::string_view kind = argc == 2 ? argv[1] : "";
    ev::Press              press{};
    if (kind == "single") {
        press = ev::Press::Single;
    } else if (kind == "double") {
        press = ev::Press::Double;
    } else if (kind == "hold") {
        press = ev::Press::Hold;
    } else {
        std::printf("usage: press single|double|hold\n");
        return 1;
    }
    g_ctx.post(ev::ButtonPressed{.press = press, .source = ev::Source::Console, .at = ev::BootClock::now()});
    return 0;
}

int cmdStatus(int, char **)
{
    const auto st = snapshot();
    if (!st) {
        return 1;
    }

    if (st->time.valid) {
        std::printf("time    %s %02d:%02d%s\n", kWeekdayNames[st->time.local.tm_wday % 7], st->time.local.tm_hour,
                    st->time.local.tm_min, st->time.simulated ? "  (simulated)" : "");
    } else {
        std::printf("time    NOT SET - waiting for time sync (lights held off)\n");
    }

    std::printf("day     ");
    printHoursMinutes(st->day.day_length_hours);
    std::printf("  (sunrise ");
    printClock(st->day.sunrise_hour);
    std::printf(", sunset ");
    printClock(st->day.sunset_hour);
    std::printf(")\nnext    %s in ", st->day.next_is_sunrise ? "sunrise" : "sunset");
    printHoursMinutes(st->day.hours_until_next);
    std::printf(" (");
    printClock(st->day.next_event_hour);
    std::printf(")\n");

    std::printf("mode    %s", ev::modeName(st->mode));
    if (st->mode == ev::Mode::Daylight) {
        std::printf(" - %s", st->in_active_window ? "in active hours" : "outside active hours, lights off");
    }
    std::printf("\n");

    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        const ev::DeviceStatus &d = st->devices[i];
        std::printf("  %-6s ", ev::roleName(i).data());
        switch (d.link) {
        case ev::DeviceLink::Unpaired:
            std::printf("-  not paired\n");
            continue;
        case ev::DeviceLink::Silent:
            std::printf("o  silent");
            break;
        case ev::DeviceLink::Online:
            std::printf("*  online");
            break;
        }
        if (d.last_heard) {
            const auto ago = std::chrono::duration_cast<std::chrono::seconds>(st->at - *d.last_heard);
            std::printf(", heard %llds ago", static_cast<long long>(ago.count()));
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

// ---- wifi / restart / factory_reset -----------------------------------------------

// Wi-Fi networks live in their own NVS keys and are only read by the
// time-sync task at connect time, so these call the store directly.
void printWifiUsage()
{
    std::printf("usage: wifi                          list saved networks\n"
                "       wifi add <ssid> [password]    add one (or change its password); up to %u\n"
                "       wifi remove <ssid>            forget one\n"
                "       wifi clear                    forget all (falls back to menuconfig's)\n"
                "quote an SSID with spaces: wifi add \"My Net\" secret\n",
                static_cast<unsigned>(kMaxWifiNetworks));
}

int addWifi(const char *ssid, const char *password)
{
    switch (addWifiNetwork(ssid, password)) {
    case WifiSaveResult::Added:
        std::printf("added \"%s\"; type `restart` to sync time now\n", ssid);
        return 0;
    case WifiSaveResult::Updated:
        std::printf("updated the password for \"%s\"\n", ssid);
        return 0;
    case WifiSaveResult::Invalid:
        std::printf("not saved: SSID must be 1-32 chars, password at most 64\n");
        return 1;
    case WifiSaveResult::Full:
        std::printf("not saved: already %u networks; `wifi remove` one first\n",
                    static_cast<unsigned>(kMaxWifiNetworks));
        return 1;
    case WifiSaveResult::StorageError:
        std::printf("not saved: storage error\n");
        return 1;
    }
    return 1;
}

int cmdWifi(int argc, char **argv)
{
    const std::string_view sub = argc >= 2 ? argv[1] : "";
    if (argc == 1) {
        const WifiNetworks saved = loadWifiNetworks();
        if (saved.networks.empty()) {
            std::printf("no Wi-Fi networks; add one with: wifi add <ssid> [password]\n");
            return 0;
        }
        if (saved.from_menuconfig) {
            std::printf("none saved; using the menuconfig fallback:\n");
        }
        for (std::size_t i = 0; i < saved.networks.size(); ++i) {
            const WifiNetwork &n = saved.networks[i];
            std::printf("  %u. \"%s\" (%s)\n", static_cast<unsigned>(i + 1), n.ssid.c_str(),
                        n.password.empty() ? "open network" : "password set");
        }
        std::printf("the strongest one in range is used at each clock sync\n");
        return 0;
    }
    if (sub == "add" && (argc == 3 || argc == 4)) {
        return addWifi(argv[2], argc == 4 ? argv[3] : "");
    }
    if (sub == "remove" && argc == 3) {
        if (!removeWifiNetwork(argv[2])) {
            std::printf("no saved network called \"%s\"\n", argv[2]);
            return 1;
        }
        std::printf("removed \"%s\"\n", argv[2]);
        return 0;
    }
    if (sub == "clear" && argc == 2) {
        clearWifiNetworks();
        std::printf("all networks forgotten; falling back to menuconfig's Wi-Fi settings\n");
        return 0;
    }
    printWifiUsage();
    return 1;
}

int cmdRestart(int, char **)
{
    esp_restart();
}

int cmdFactoryReset(int, char **)
{
    g_ctx.post(ev::FactoryReset{});
    return 0;
}

void registerCommand(const char *name, const char *help, const char *hint, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd{
        .command        = name,
        .help           = help,
        .hint           = hint,
        .func           = func,
        .argtable       = nullptr,
        .func_w_context = nullptr,
        .context        = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
} // namespace

void startConsole(const ev::Post &post, const StatusSlot &status)
{
    g_ctx = {.post = post, .status = &status};

    esp_console_repl_t       *repl        = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt                    = "sunsim>";
    repl_config.task_stack_size           = 6144; // commands post events and copy snapshots

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl));
#else
#error "the console needs a UART or USB-Serial-JTAG primary console"
#endif

    ESP_ERROR_CHECK(esp_console_register_help_command());
    registerCommand("status", "Time, day length, next sunrise/sunset, mode and device links", nullptr, &cmdStatus);
    registerCommand("roles", "List role assignments", nullptr, &cmdRoles);
    registerCommand("pair", "Give the next unassigned device to join this role, and open the network",
                    "<WallA|WallB|WallC|Work|Button|cancel>", &cmdPair);
    registerCommand("unpair", "Forget a role's device (it can then be re-paired)", "<role|all>", &cmdUnpair);
    registerCommand("open", "Open the network for joining", "[seconds]", &cmdOpen);
    registerCommand("config", "Show or change active/work hours, fade, sun shift, amber glow and work days",
                    "[hours|workhours HH:MM-HH:MM | fade <min> | sunshift <0-100> | amber on|off | days ...]",
                    &cmdConfig);
    registerCommand("intensity", "Show or set each light's peak brightness, 1-100%", "[<light|all> <1-100>]",
                    &cmdIntensity);
    registerCommand("press", "Simulate the button: single = resume daylight, double = red alert, hold = all off",
                    "single|double|hold", &cmdPress);
    registerCommand("wifi", "List, add or remove the Wi-Fi networks used for the clock sync (e.g. home and work)",
                    "[add <ssid> [password] | remove <ssid> | clear]", &cmdWifi);
    registerCommand("restart", "Reboot (keeps roles, network and Wi-Fi settings)", nullptr, &cmdRestart);
    registerCommand("factory_reset", "Forget all roles AND the Zigbee network, then restart", nullptr,
                    &cmdFactoryReset);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

} // namespace sunsim::platform
