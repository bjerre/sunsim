module;

#include "esp_console.h"
#include "esp_system.h"
#include "sdkconfig.h"

export module sunsim.platform:console;

import std.compat;
import sunsim.ev;
import :status_slot;
import :format;
import :wifi;

export namespace sunsim::platform {

// Serial (idf.py monitor) REPL. Commands post requests as events and read
// state from the latest StatusSnapshot; they never touch app state directly.
//
//   status                time, day length, next sunrise/sunset, mode, device links
//   roles                 list role assignments
//   pair <role>           next unassigned device to join gets <role>; opens the network
//   pair cancel           disarm a pending `pair`
//   unpair <role|group|all>   forget role assignment(s)
//   open [seconds]        open the network for joining (default 180)
//   config                show the settings (ev::kSettings)
//   config <key> <value>  change one (`config x` lists what each takes)
//   intensity [<light|group|all> <1-100>]   per-light peak brightness, saved
//   minlevel [<light|group|all> <1-254>]    per-light minimum level while on, saved
//   tune [<light|group|all> <1-254> [kelvin] | save | off]   find each bulb's lowest visible level
//   press single|double|hold          simulate the button
//   fastday [on [seconds] | off]      the debug fast-day clock (a whole day in seconds)
//   sync                  sync the clock (and fetch internet data) now
//   weather               the weather now: cloud, rain or snow, alerts, and the weather lights
//   weather simulate <what>   pretend: rain|snow|... [heavy], advisory|watch|warning, or clear
//   wifi                  list saved Wi-Fi networks (the strongest in range is used)
//   wifi add <ssid> [pw]  add a network (up to 5), or change its password
//   wifi remove <ssid>    forget one; `wifi clear` forgets all
//   restart               reboot, keeping everything
//   factory_reset         forget all roles AND the Zigbee network, then restart
//
// <role> is a device's name (WallA-WallE east to west, Work, Button); a
// <group> is all of one kind (wall). Case doesn't matter.
class Console {
public:
    Console(ev::Post post, const StatusSlot &status) : post_(std::move(post)), status_(status) {}

    Console(const Console &)            = delete;
    Console &operator=(const Console &) = delete;

    // Registers the commands and starts the REPL on its own task.
    void start();

    // For the commands.
    void post(ev::Event e) const { post_(std::move(e)); }
    // The latest status, or nullopt after saying it's still starting up.
    [[nodiscard]] std::optional<ev::StatusSnapshot> snapshot() const;

private:
    ev::Post          post_;
    const StatusSlot &status_;
};

} // namespace sunsim::platform

namespace sunsim::platform {

std::optional<ev::StatusSnapshot> Console::snapshot() const
{
    auto s = status_.get();
    if (!s) {
        std::printf("still starting up; try again in a moment\n");
    }
    return s;
}

namespace {

void printRoleNames()
{
    std::printf("roles:");
    for (const ev::Role role : ev::kRoles) {
        std::printf(" %s", ev::roleName(role).data());
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

// The role, group or "all" named by `arg`, or nullopt after saying why not.
std::optional<ev::Selection> parseSelectionArg(const char *arg)
{
    const auto selection = ev::parseSelection(arg);
    if (!selection) {
        std::printf("unknown role or group '%s'\n", arg);
        printRoleNames();
        std::printf("groups:");
        for (const ev::GroupSpec &g : ev::kLayout) {
            std::printf(" %.*s", static_cast<int>(g.name.size()), g.name.data());
        }
        std::printf(" (or all)\n");
    }
    return selection;
}

// ---- roles / pair / unpair / open ---------------------------------------------

int cmdRoles(Console &console, int, char **)
{
    const auto st = console.snapshot();
    if (!st) {
        return 1;
    }
    for (const ev::Role role : ev::kRoles) {
        const ev::DeviceRecord &d = st->records[role];
        if (d.valid) {
            std::printf("%-6s ext=0x%016llx short=0x%04x ep=%d\n", ev::roleName(role).data(),
                        static_cast<unsigned long long>(d.ext_addr), d.short_addr, d.endpoint);
        } else {
            std::printf("%-6s (unassigned)\n", ev::roleName(role).data());
        }
    }
    if (st->armed) {
        std::printf("Armed: next joining device becomes %s\n", ev::roleName(*st->armed).data());
    }
    return 0;
}

int cmdPair(Console &console, int argc, char **argv)
{
    if (argc != 2) {
        std::printf("usage: pair <role>|cancel\n");
        printRoleNames();
        return 1;
    }
    if (std::string_view{argv[1]} == "cancel") {
        console.post(ev::PairCancelled{});
        return 0;
    }
    const auto role = parseRoleArg(argv[1]);
    if (!role) {
        return 1;
    }
    console.post(ev::PairRequested{*role});
    return 0;
}

int cmdUnpair(Console &console, int argc, char **argv)
{
    if (argc != 2) {
        std::printf("usage: unpair <role|group|all>\n");
        printRoleNames();
        return 1;
    }
    const auto roles = parseSelectionArg(argv[1]);
    if (!roles) {
        return 1;
    }
    console.post(ev::UnpairRequested{*roles});
    return 0;
}

int cmdOpen(Console &console, int argc, char **argv)
{
    const int seconds = argc == 2 ? std::atoi(argv[1]) : ev::kDefaultOpenTime.count();
    if (seconds < 1 || seconds > 254) {
        std::printf("seconds must be 1-254\n");
        return 1;
    }
    console.post(ev::OpenNetwork{ev::OpenTime{seconds}});
    return 0;
}

// ---- config ---------------------------------------------------------------------

// Every setting is in ev::kSettings: what it's called, takes and means.

void printSetting(const ev::Setting &setting, const ev::ProgramConfig &c)
{
    const std::string value = ev::show(setting, c);
    std::printf("  %-11.*s %-14s %.*s\n", static_cast<int>(setting.name.size()), setting.name.data(), value.c_str(),
                static_cast<int>(setting.help.size()), setting.help.data());
}

void printConfigUsage()
{
    std::printf("usage: config [<setting> <value>]\n");
    for (const ev::Setting &setting : ev::kSettings) {
        const std::string takes = ev::syntax(setting);
        std::printf("  %-11.*s %s\n", static_cast<int>(setting.name.size()), setting.name.data(), takes.c_str());
    }
}

int cmdConfig(Console &console, int argc, char **argv)
{
    const auto st = console.snapshot();
    if (!st) {
        return 1;
    }
    ev::ProgramConfig c = st->config;
    if (argc == 1) {
        for (const ev::Setting &setting : ev::kSettings) {
            printSetting(setting, c);
        }
        return 0;
    }
    const ev::Setting *setting = argc == 3 ? ev::findSetting(argv[1]) : nullptr;
    if (setting == nullptr) {
        printConfigUsage();
        return 1;
    }
    if (const auto done = ev::set(*setting, c, argv[2]); !done) {
        std::printf("%s\n", done.error().c_str());
        return 1;
    }
    console.post(ev::SetConfig{c});
    printSetting(*setting, c);
    return 0;
}

// ---- intensity --------------------------------------------------------------------

void printPeaks(const ev::Peaks &peaks)
{
    for (const ev::Role role : ev::bulbs()) {
        std::printf("  %-6s %3u%%\n", ev::roleName(role).data(), peaks[role]);
    }
}

void printMinLevels(const ev::MinLevels &levels)
{
    for (const ev::Role role : ev::bulbs()) {
        std::printf("  %-6s %3u\n", ev::roleName(role).data(), levels[role]);
    }
}

int cmdIntensity(Console &console, int argc, char **argv)
{
    const auto st = console.snapshot();
    if (!st) {
        return 1;
    }
    if (argc == 1) {
        printPeaks(st->peaks);
        return 0;
    }
    const int percent = argc == 3 ? std::atoi(argv[2]) : 0;
    if (argc != 3 || percent < 1 || percent > 100) {
        std::printf("usage: intensity [<light|group|all> <1-100>]   e.g. intensity WallC 80, intensity wall 60\n");
        return 1;
    }
    const auto lights = parseSelectionArg(argv[1]);
    if (!lights) {
        return 1;
    }
    if (std::ranges::none_of(ev::bulbs(), [&](ev::Role role) { return ev::selects(*lights, role); })) {
        std::printf("'%s' isn't a light\n", argv[1]);
        return 1;
    }

    console.post(ev::SetIntensity{*lights, static_cast<uint8_t>(percent)});
    ev::Peaks peaks = st->peaks;
    for (const ev::Role role : ev::bulbs()) {
        if (ev::selects(*lights, role)) {
            peaks[role] = static_cast<uint8_t>(percent);
        }
    }
    printPeaks(peaks);
    return 0;
}

int cmdMinLevel(Console &console, int argc, char **argv)
{
    const auto st = console.snapshot();
    if (!st) {
        return 1;
    }
    if (argc == 1) {
        printMinLevels(st->min_levels);
        return 0;
    }
    const int level = argc == 3 ? std::atoi(argv[2]) : 0;
    if (argc != 3 || level < 1 || level > ev::kMaxLevel) {
        std::printf("usage: minlevel [<light|group|all> <1-254>]   (find them with `tune`)\n");
        return 1;
    }
    const auto lights = parseSelectionArg(argv[1]);
    if (!lights) {
        return 1;
    }
    console.post(ev::SetMinLevel{*lights, static_cast<uint8_t>(level)});
    ev::MinLevels levels = st->min_levels;
    for (const ev::Role role : ev::bulbs()) {
        if (ev::selects(*lights, role)) {
            levels[role] = static_cast<uint8_t>(level);
        }
    }
    printMinLevels(levels);
    return 0;
}

int cmdTune(Console &console, int argc, char **argv)
{
    const auto st = console.snapshot();
    if (!st) {
        return 1;
    }
    const bool             tuning = st->mode == ev::Mode::Tuning;
    const std::string_view what   = argc >= 2 ? argv[1] : "";
    if (argc == 1) {
        if (!tuning) {
            console.post(ev::ModeRequested{ev::Mode::Tuning});
        }
        std::printf("tuning: every bulb steady at its minimum level, in sunset amber. Raise a bulb until it\n"
                    "shows, then `tune save`; `tune off` (or a button press) returns to daylight.\n"
                    "  tune <light|group|all> <1-254> [kelvin]\nminimum levels now:\n");
        printMinLevels(st->min_levels);
        return 0;
    }
    if (what == "off" && argc == 2) {
        console.post(ev::ModeRequested{ev::Mode::Daylight});
        return 0;
    }
    if (what == "save" && argc == 2) {
        if (!tuning) {
            std::printf("not tuning: nothing to save (start with `tune`)\n");
            return 1;
        }
        console.post(ev::TuneSave{});
        std::printf("saved as the minimum levels (see `minlevel`)\n");
        return 0;
    }
    const int level  = argc >= 3 ? std::atoi(argv[2]) : 0;
    const int kelvin = argc == 4 ? std::atoi(argv[3]) : 0;
    if (argc < 3 || argc > 4 || level < 1 || level > ev::kMaxLevel || (argc == 4 && (kelvin < 1700 || kelvin > 6500))) {
        std::printf("usage: tune [<light|group|all> <1-254> [1700-6500 kelvin] | save | off]\n");
        return 1;
    }
    const auto lights = parseSelectionArg(argv[1]);
    if (!lights) {
        return 1;
    }
    if (!tuning) {
        console.post(ev::ModeRequested{ev::Mode::Tuning});
    }
    console.post(ev::TuneLights{.lights = *lights,
                              .level  = static_cast<uint8_t>(level),
                              .kelvin = argc == 4 ? std::optional<uint16_t>{static_cast<uint16_t>(kelvin)} : std::nullopt});
    return 0;
}

// ---- press / status ---------------------------------------------------------------

// Posts the same event the real button does, so this exercises the whole
// press -> mode -> lights path.
int cmdPress(Console &console, int argc, char **argv)
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
    console.post(ev::ButtonPressed{.press = press, .source = ev::Source::Console, .at = ev::BootClock::now()});
    return 0;
}

// ---- fastday / sync -------------------------------------------------------------

int cmdFastDay(Console &console, int argc, char **argv)
{
    const std::string_view what = argc >= 2 ? argv[1] : "";
    if (argc == 1) {
        const auto st = console.snapshot();
        if (!st) {
            return 1;
        }
        std::printf("fast-day %s\n", st->time.simulated ? "on" : "off");
        return 0;
    }
    if (what == "off" && argc == 2) {
        console.post(ev::SetFastDay{.on = false});
        return 0;
    }
    if (what == "on" && argc <= 3) {
        const int seconds = argc == 3 ? std::atoi(argv[2]) : CONFIG_SUNSIM_DEBUG_DAY_SECONDS;
        if (seconds < 10 || seconds > 3600) {
            std::printf("seconds per day must be 10-3600\n");
            return 1;
        }
        console.post(ev::SetFastDay{.on = true, .day_length = std::chrono::seconds{seconds}});
        return 0;
    }
    std::printf("usage: fastday [on [seconds] | off]   (on: one active window every %d s by default)\n",
                CONFIG_SUNSIM_DEBUG_DAY_SECONDS);
    return 1;
}

int cmdSync(Console &console, int, char **)
{
    console.post(ev::SyncRequested{});
    std::printf("syncing in the background (see the INTERNET log lines)\n");
    return 0;
}

// ---- weather -------------------------------------------------------------------

void printWeather(const ev::StatusSnapshot &st)
{
    if (st.sky.conditions) {
        std::printf("now      %u%% cloud, ", st.sky.conditions->cloud_percent);
        if (st.sky.conditions->precip == ev::Precip::None) {
            std::printf("nothing falling\n");
        } else {
            std::printf("%s %s\n", ev::intensityName(st.sky.conditions->intensity), ev::precipName(st.sky.conditions->precip));
        }
    } else {
        std::printf("now      no live conditions (none fetched in the last %lld min)\n",
                    static_cast<long long>(ev::kWeatherFreshFor.count()));
    }
    if (st.sky.alerts.count == 0) {
        std::printf("alerts   none\n");
    }
    for (const ev::Alert &alert : st.sky.alerts.list()) {
        const auto left = std::chrono::duration_cast<std::chrono::minutes>(alert.until - st.at);
        std::printf("alert    %s (%s), %lld min left\n", alert.event.data(), ev::alertLevelName(alert.level),
                    static_cast<long long>(left.count()));
    }
    for (const std::string_view name : {"weather", "precip", "alerts"}) {
        printSetting(*ev::findSetting(name), st.config);
    }
}

// What `weather simulate` takes for rain and snow.
constexpr std::pair<std::string_view, ev::Precip> kPrecipNames[] = {
    {"drizzle", ev::Precip::Drizzle}, {"rain", ev::Precip::Rain},       {"freezing", ev::Precip::FreezingRain},
    {"snow", ev::Precip::Snow},       {"thunder", ev::Precip::Thunderstorm},
};
constexpr std::pair<std::string_view, ev::AlertLevel> kAlertNames[] = {
    {"advisory", ev::AlertLevel::Advisory}, {"watch", ev::AlertLevel::Watch}, {"warning", ev::AlertLevel::Warning}};

template <class T, std::size_t N>
std::optional<T> lookup(const std::pair<std::string_view, T> (&names)[N], std::string_view name)
{
    const auto *found = std::ranges::find(names, name, &std::pair<std::string_view, T>::first);
    return found == std::end(names) ? std::nullopt : std::optional<T>{found->second};
}

// Posts what InternetSync would: it lasts until the next real fetch (within
// 15 minutes), or `weather simulate clear`.
int simulateWeather(Console &console, const ev::StatusSnapshot &st, std::string_view what, std::string_view how)
{
    const ev::Instant now   = ev::BootClock::now();
    const uint8_t     cloud = st.sky.conditions ? st.sky.conditions->cloud_percent : st.sky.cloud_percent.value_or(0);
    if (what == "clear") {
        console.post(ev::ConditionsUpdated{.now = {.cloud_percent = cloud}, .at = now});
        console.post(ev::AlertsUpdated{.at = now});
        return 0;
    }
    if (const auto precip = lookup(kPrecipNames, what)) {
        const auto intensity = how == "light" ? ev::Intensity::Light
                               : how == "heavy" ? ev::Intensity::Heavy
                                                : ev::Intensity::Moderate;
        console.post(ev::ConditionsUpdated{
            .now = {.cloud_percent = std::max<uint8_t>(cloud, 90), .precip = *precip, .intensity = intensity}, .at = now});
        return 0;
    }
    if (const auto level = lookup(kAlertNames, what)) {
        ev::AlertsUpdated alerts{.at = now};
        ev::Alert        &alert = alerts.alerts.items[alerts.alerts.count++];
        alert.level             = *level;
        alert.until             = now + std::chrono::hours{1};
        std::snprintf(alert.event.data(), alert.event.size(), "Simulated %s", ev::alertLevelName(*level));
        console.post(alerts);
        return 0;
    }
    std::printf("usage: weather simulate drizzle|rain|freezing|snow|thunder [light|moderate|heavy]\n"
                "       weather simulate advisory|watch|warning | clear\n"
                "(until the next real fetch, within 15 min)\n");
    return 1;
}

int cmdWeather(Console &console, int argc, char **argv)
{
    const auto st = console.snapshot();
    if (!st) {
        return 1;
    }
    if (argc == 1) {
        printWeather(*st);
        return 0;
    }
    if (std::string_view{argv[1]} == "simulate" && (argc == 3 || argc == 4)) {
        return simulateWeather(console, *st, argv[2], argc == 4 ? argv[3] : "");
    }
    std::printf("usage: weather [simulate <what>]\n");
    return 1;
}

int cmdStatus(Console &console, int, char **)
{
    const auto st = console.snapshot();
    if (!st) {
        return 1;
    }

    if (st->time.valid) {
        const std::string now = std::format("{:%a %H:%M}", st->time.local);
        std::printf("time    %s%s\n", now.c_str(), st->time.simulated ? "  (simulated)" : "");
    } else {
        std::printf("time    NOT SET - waiting for time sync (lights held off)\n");
    }

    const ev::DayInfo &day = st->day;
    std::printf("day     %s  (sunrise %s, sunset %s)\n", durationText(day.day_length).c_str(),
                clockText(day.sunrise).c_str(), clockText(day.sunset).c_str());
    std::printf("next    %s in %s (%s)\n", day.next_is_sunrise ? "sunrise" : "sunset", durationText(day.until_next).c_str(),
                clockText(day.next_event).c_str());

    if (st->sky.cloud_percent) {
        std::printf("sky     %u%% cloud (%s)%s\n", *st->sky.cloud_percent, st->sky.cloud_live ? "now" : "forecast",
                    st->config.clouds_enabled ? "" : " (clouds off)");
    } else {
        std::printf("sky     no forecast yet (clear assumed)\n");
    }
    if (st->sky.conditions && st->sky.conditions->precip != ev::Precip::None) {
        std::printf("falling %s %s\n", ev::intensityName(st->sky.conditions->intensity), ev::precipName(st->sky.conditions->precip));
    }
    for (const ev::Alert &alert : st->sky.alerts.list()) {
        std::printf("alert   %s\n", alert.event.data());
    }

    std::printf("mode    %s", ev::modeName(st->mode));
    if (st->mode == ev::Mode::Daylight) {
        std::printf(" - %s", st->in_active_window ? "in active hours" : "outside active hours, lights off");
    }
    std::printf("\n");

    for (const ev::Role role : ev::kRoles) {
        const ev::DeviceStatus &d = st->devices[role];
        std::printf("  %-6s ", ev::roleName(role).data());
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

int cmdWifi(Console &, int argc, char **argv)
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

int cmdRestart(Console &, int, char **)
{
    esp_restart();
}

int cmdFactoryReset(Console &console, int, char **)
{
    console.post(ev::FactoryReset{});
    return 0;
}

// A command: what it's given, and esp_console's exit status.
using Command = int (*)(Console &, int, char **);

// Registers `command` under `name`, reaching it through esp_console's
// context pointer: no globals.
template <Command command> void add(Console &console, const char *name, const char *help, const char *hint)
{
    const esp_console_cmd_t cmd{
        .command        = name,
        .help           = help,
        .hint           = hint,
        .func           = nullptr,
        .argtable       = nullptr,
        .func_w_context = [](void *context, int argc, char **argv) {
            return command(*static_cast<Console *>(context), argc, argv);
        },
        .context = &console,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
} // namespace

void Console::start()
{
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
    add<&cmdStatus>(*this, "status", "Time, day length, next sunrise/sunset, mode and device links", nullptr);
    add<&cmdRoles>(*this, "roles", "List role assignments", nullptr);
    add<&cmdPair>(*this, "pair", "Give the next unassigned device to join this role, and open the network",
                  "<role|cancel>");
    add<&cmdUnpair>(*this, "unpair", "Forget a role's device, or a group's (they can then be re-paired)",
                    "<role|group|all>");
    add<&cmdOpen>(*this, "open", "Open the network for joining", "[seconds]");
    add<&cmdConfig>(*this, "config", "Show the settings, or change one (`config x` lists what each takes)",
                    "[<setting> <value>]");
    add<&cmdIntensity>(*this, "intensity", "Show or set each light's peak brightness, 1-100%",
                       "[<light|group|all> <1-100>]");
    add<&cmdMinLevel>(*this, "minlevel", "Show or set each light's minimum level while on, 1-254 (find them with tune)",
                      "[<light|group|all> <1-254>]");
    add<&cmdTune>(*this, "tune", "Tuning mode: hold bulbs steady to find each one's lowest visible level",
                  "[<light|group|all> <1-254> [kelvin] | save | off]");
    add<&cmdPress>(*this, "press", "Simulate the button: single = resume daylight, double = red alert, hold = all off",
                   "single|double|hold");
    add<&cmdFastDay>(*this, "fastday", "Debug: run a whole active window every few seconds, or back to real time",
                     "[on [seconds] | off]");
    add<&cmdSync>(*this, "sync", "Sync the clock (and fetch internet data) now, over Wi-Fi", nullptr);
    add<&cmdWeather>(*this, "weather", "The weather now and the weather lights; or pretend, to see the lights",
                     "[simulate drizzle|rain|freezing|snow|thunder [light|heavy] | advisory|watch|warning | clear]");
    add<&cmdWifi>(*this, "wifi", "List, add or remove the Wi-Fi networks used for the internet sync (e.g. home and work)",
                  "[add <ssid> [password] | remove <ssid> | clear]");
    add<&cmdRestart>(*this, "restart", "Reboot (keeps roles, network and Wi-Fi settings)", nullptr);
    add<&cmdFactoryReset>(*this, "factory_reset", "Forget all roles AND the Zigbee network, then restart", nullptr);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

} // namespace sunsim::platform
