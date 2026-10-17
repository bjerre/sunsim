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
        std::println("still starting up; try again in a moment");
    }
    return s;
}

namespace {

// A whole number from lo to hi, all of `text`: not "12x", "" or "-".
std::optional<int> numberIn(std::string_view text, int lo, int hi) noexcept
{
    int value = 0;
    const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (err != std::errc{} || end != text.data() + text.size() || value < lo || value > hi) {
        return std::nullopt;
    }
    return value;
}

void printRoleNames()
{
    std::print("roles:");
    for (const ev::Role role : ev::kRoles) {
        std::print(" {}", ev::roleName(role));
    }
    std::println();
}

// The role named by `arg`, or nullopt after saying why not.
std::optional<ev::Role> parseRoleArg(const char *arg)
{
    const auto role = ev::parseRole(arg);
    if (!role) {
        std::println("unknown role '{}'", arg);
        printRoleNames();
    }
    return role;
}

// The role, group or "all" named by `arg`, or nullopt after saying why not.
std::optional<ev::Selection> parseSelectionArg(const char *arg)
{
    const auto selection = ev::parseSelection(arg);
    if (!selection) {
        std::println("unknown role or group '{}'", arg);
        printRoleNames();
        std::print("groups:");
        for (const ev::GroupSpec &g : ev::kLayout) {
            std::print(" {}", g.name);
        }
        std::println(" (or all)");
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
            std::println("{:<6} ext=0x{:016x} short=0x{:04x} ep={}", ev::roleName(role),
                         d.ext_addr, d.short_addr, d.endpoint);
        } else {
            std::println("{:<6} (unassigned)", ev::roleName(role));
        }
    }
    if (st->armed) {
        std::println("Armed: next joining device becomes {}", ev::roleName(*st->armed));
    }
    return 0;
}

int cmdPair(Console &console, int argc, char **argv)
{
    if (argc != 2) {
        std::println("usage: pair <role>|cancel");
        printRoleNames();
        return 1;
    }
    if (std::string_view{argv[1]} == "cancel") {
        console.post(ev::PairCanceled{});
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
        std::println("usage: unpair <role|group|all>");
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
    const auto seconds = argc == 2 ? numberIn(argv[1], 1, 254) : std::optional<int>{ev::kDefaultOpenTime.count()};
    if (!seconds) {
        std::println("seconds must be 1-254");
        return 1;
    }
    console.post(ev::OpenNetwork{ev::OpenTime{*seconds}});
    return 0;
}

// ---- config ---------------------------------------------------------------------

// Every setting is in ev::kSettings: what it's called, takes and means.

void printSetting(const ev::Setting &setting, const ev::ProgramConfig &c)
{
    const std::string value = ev::show(setting, c);
    std::println("  {:<11} {:<14} {}", setting.name, value,
                 setting.help);
}

void printConfigUsage()
{
    std::println("usage: config [<setting> <value>]");
    for (const ev::Setting &setting : ev::kSettings) {
        const std::string takes = ev::syntax(setting);
        std::println("  {:<11} {}", setting.name, takes);
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
        std::println("{}", done.error());
        return 1;
    }
    console.post(ev::SetConfig{c});
    printSetting(*setting, c);
    return 0;
}

// ---- intensity --------------------------------------------------------------------

void printPeaks(const ev::Peaks &peaks)
{
    for (const ev::Role role : ev::lights()) {
        std::println("  {:<6} {:3}%", ev::roleName(role), peaks[role]);
    }
}

void printMinLevels(const ev::MinLevels &levels)
{
    for (const ev::Role role : ev::lights()) {
        std::println("  {:<6} {:3}", ev::roleName(role), levels[role]);
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
    const auto percent = argc == 3 ? numberIn(argv[2], 1, 100) : std::nullopt;
    if (!percent) {
        std::println("usage: intensity [<light|group|all> <1-100>]   e.g. intensity WallC 80, intensity wall 60");
        return 1;
    }
    const auto lights = parseSelectionArg(argv[1]);
    if (!lights) {
        return 1;
    }
    if (std::ranges::none_of(ev::lights(), [&](ev::Role role) { return ev::selects(*lights, role); })) {
        std::println("'{}' isn't a light", argv[1]);
        return 1;
    }

    console.post(ev::SetIntensity{*lights, static_cast<uint8_t>(*percent)});
    ev::Peaks peaks = st->peaks;
    for (const ev::Role role : ev::lights()) {
        if (ev::selects(*lights, role)) {
            peaks[role] = static_cast<uint8_t>(*percent);
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
    const auto level = argc == 3 ? numberIn(argv[2], 1, ev::kMaxLevel) : std::nullopt;
    if (!level) {
        std::println("usage: minlevel [<light|group|all> <1-254>]   (find them with `tune`)");
        return 1;
    }
    const auto lights = parseSelectionArg(argv[1]);
    if (!lights) {
        return 1;
    }
    console.post(ev::SetMinLevel{*lights, static_cast<uint8_t>(*level)});
    ev::MinLevels levels = st->min_levels;
    for (const ev::Role role : ev::lights()) {
        if (ev::selects(*lights, role)) {
            levels[role] = static_cast<uint8_t>(*level);
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
    const bool tuning =
        std::ranges::any_of(st->zones.items, [](const ev::ZoneStatus &z) { return z.mode == ev::Mode::Tuning; });
    const std::string_view what = argc >= 2 ? argv[1] : "";
    if (argc == 1) {
        if (!tuning) {
            console.post(ev::ModeRequested{ev::Mode::Tuning});
        }
        std::print("tuning: every bulb steady at its minimum level, in sunset amber. Raise a bulb until it\n"
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
            std::println("not tuning: nothing to save (start with `tune`)");
            return 1;
        }
        console.post(ev::TuneSave{});
        std::println("saved as the minimum levels (see `minlevel`)");
        return 0;
    }
    const auto level  = argc >= 3 ? numberIn(argv[2], 1, ev::kMaxLevel) : std::nullopt;
    const auto kelvin = argc == 4 ? numberIn(argv[3], 1700, 6500) : std::nullopt;
    if (argc > 4 || !level || (argc == 4 && !kelvin)) {
        std::println("usage: tune [<light|group|all> <1-254> [1700-6500 kelvin] | save | off]");
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
                              .level  = static_cast<uint8_t>(*level),
                              .kelvin = kelvin.transform([](int k) { return static_cast<uint16_t>(k); })});
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
        std::println("usage: press single|double|hold");
        return 1;
    }
    const auto button = std::ranges::find_if(ev::kRoles, ev::isButton);
    if (button == ev::kRoles.end()) {
        std::println("there's no button in the layout");
        return 1;
    }
    console.post(ev::ButtonPressed{.button = *button, .press = press, .source = ev::Source::Console,
                                   .at = ev::BootClock::now()});
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
        std::println("fast-day {}", st->time.simulated ? "on" : "off");
        return 0;
    }
    if (what == "off" && argc == 2) {
        console.post(ev::SetFastDay{.on = false});
        return 0;
    }
    if (what == "on" && argc <= 3) {
        const auto seconds = argc == 3 ? numberIn(argv[2], 10, 3600) : std::optional<int>{CONFIG_SUNSIM_DEBUG_DAY_SECONDS};
        if (!seconds) {
            std::println("seconds per day must be 10-3600");
            return 1;
        }
        console.post(ev::SetFastDay{.on = true, .day_length = std::chrono::seconds{*seconds}});
        return 0;
    }
    std::println("usage: fastday [on [seconds] | off]   (on: one active window every {} s by default)",
                 CONFIG_SUNSIM_DEBUG_DAY_SECONDS);
    return 1;
}

int cmdSync(Console &console, int, char **)
{
    console.post(ev::SyncRequested{});
    std::println("syncing in the background (see the INTERNET log lines)");
    return 0;
}

// ---- weather -------------------------------------------------------------------

// "WallB,WallD", or "none".
std::string lightsText(const ev::RoleSet &set)
{
    std::string text;
    for (const ev::Role role : ev::lights()) {
        if (ev::contains(set, role)) {
            text += text.empty() ? "" : ",";
            text += ev::roleName(role);
        }
    }
    return text.empty() ? "none" : text;
}

void printWeather(const ev::StatusSnapshot &st)
{
    if (st.sky.conditions) {
        std::print("now      {}% cloud, ", st.sky.conditions->cloud_percent);
        if (st.sky.conditions->precip == ev::Precip::None) {
            std::println("nothing falling");
        } else {
            std::println("{} {}", ev::intensityName(st.sky.conditions->intensity), ev::precipName(st.sky.conditions->precip));
        }
    } else {
        std::println("now      no live conditions (none fetched in the last {} min)",
                     ev::kWeatherFreshFor.count());
    }
    if (st.sky.alerts.count == 0) {
        std::println("alerts   none");
    }
    for (const ev::Alert &alert : st.sky.alerts.list()) {
        const auto left = std::chrono::duration_cast<std::chrono::minutes>(alert.until - st.at);
        std::println("alert    {} ({}), {} min left", alert.event.data(), ev::alertLevelName(alert.level),
                     left.count());
    }
    printSetting(*ev::findSetting("weather"), st.config);
    std::println("precip   {}", lightsText(ev::kWeatherLights.precip));
    std::println("alerts   {}", lightsText(ev::kWeatherLights.alerts));
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
        std::format_to_n(alert.event.data(), alert.event.size() - 1, "Simulated {}", ev::alertLevelName(*level)); // stays NUL-terminated
        console.post(alerts);
        return 0;
    }
    std::print("usage: weather simulate drizzle|rain|freezing|snow|thunder [light|moderate|heavy]\n"
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
    std::println("usage: weather [simulate <what>]");
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
        std::println("time    {}{}", now, st->time.simulated ? "  (simulated)" : "");
    } else {
        std::println("time    NOT SET - waiting for time sync (lights held off)");
    }

    const ev::DayInfo &day = st->day;
    std::println("day     {}  (sunrise {}, sunset {})", durationText(day.day_length),
                 clockText(day.sunrise), clockText(day.sunset));
    std::println("next    {} in {} ({})", day.next_is_sunrise ? "sunrise" : "sunset", durationText(day.until_next),
                 clockText(day.next_event));

    if (st->sky.cloud_percent) {
        std::println("sky     {}% cloud ({}){}", *st->sky.cloud_percent, st->sky.cloud_live ? "now" : "forecast",
                     st->config.clouds_enabled ? "" : " (clouds off)");
    } else {
        std::println("sky     no forecast yet (clear assumed)");
    }
    if (st->sky.conditions && st->sky.conditions->precip != ev::Precip::None) {
        std::println("falling {} {}", ev::intensityName(st->sky.conditions->intensity), ev::precipName(st->sky.conditions->precip));
    }
    for (const ev::Alert &alert : st->sky.alerts.list()) {
        std::println("alert   {}", alert.event.data());
    }

    for (const ev::ZoneId zone : ev::kZoneIds) {
        const ev::ZoneStatus &z = st->zones[zone];
        std::print("mode    ");
        if (ev::kZoneCount > 1) {
            std::print("{}: ", ev::spec(zone).name);
        }
        std::print("{}", ev::modeName(z.mode));
        if (z.mode == ev::Mode::Daylight) {
            std::print(" - {}", z.gate_open ? "lights may be on (gate open)" : "lights off (gate shut)");
        }
        std::println();
    }

    for (const ev::Role role : ev::kRoles) {
        const ev::DeviceStatus &d = st->devices[role];
        std::print("  {:<6} ", ev::roleName(role));
        switch (d.link) {
        case ev::DeviceLink::Unpaired:
            std::println("-  not paired");
            continue;
        case ev::DeviceLink::Silent:
            std::print("o  silent");
            break;
        case ev::DeviceLink::Online:
            std::print("*  online");
            break;
        }
        if (d.last_heard) {
            const auto ago = std::chrono::duration_cast<std::chrono::seconds>(st->at - *d.last_heard);
            std::print(", heard {}s ago", ago.count());
        }
        if (d.rssi != 0) {
            std::print(", rssi {}", d.rssi);
        }
        if (!d.has_endpoint) {
            std::print(", endpoint not found yet");
        }
        std::println();
    }
    return 0;
}

// ---- wifi / restart / factory_reset -----------------------------------------------

// Wi-Fi networks live in their own NVS keys and are only read by the
// time-sync task at connect time, so these call the store directly.
void printWifiUsage()
{
    std::print("usage: wifi                          list saved networks\n"
               "       wifi add <ssid> [password]    add one (or change its password); up to {}\n"
               "       wifi remove <ssid>            forget one\n"
               "       wifi clear                    forget all (falls back to menuconfig's)\n"
               "quote an SSID with spaces: wifi add \"My Net\" secret\n",
               kMaxWifiNetworks);
}

int addWifi(const char *ssid, const char *password)
{
    switch (addWifiNetwork(ssid, password)) {
    case WifiSaveResult::Added:
        std::println("added \"{}\"; type `restart` to sync time now", ssid);
        return 0;
    case WifiSaveResult::Updated:
        std::println("updated the password for \"{}\"", ssid);
        return 0;
    case WifiSaveResult::Invalid:
        std::println("not saved: SSID must be 1-32 chars, password at most 64");
        return 1;
    case WifiSaveResult::Full:
        std::println("not saved: already {} networks; `wifi remove` one first",
                     kMaxWifiNetworks);
        return 1;
    case WifiSaveResult::StorageError:
        std::println("not saved: storage error");
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
            std::println("no Wi-Fi networks; add one with: wifi add <ssid> [password]");
            return 0;
        }
        if (saved.from_menuconfig) {
            std::println("none saved; using the menuconfig fallback:");
        }
        for (std::size_t i = 0; i < saved.networks.size(); ++i) {
            const WifiNetwork &n = saved.networks[i];
            std::println("  {}. \"{}\" ({})", i + 1, n.ssid,
                         n.password.empty() ? "open network" : "password set");
        }
        std::println("the strongest one in range is used at each clock sync");
        return 0;
    }
    if (sub == "add" && (argc == 3 || argc == 4)) {
        return addWifi(argv[2], argc == 4 ? argv[3] : "");
    }
    if (sub == "remove" && argc == 3) {
        if (!removeWifiNetwork(argv[2])) {
            std::println("no saved network called \"{}\"", argv[2]);
            return 1;
        }
        std::println("removed \"{}\"", argv[2]);
        return 0;
    }
    if (sub == "clear" && argc == 2) {
        clearWifiNetworks();
        std::println("all networks forgotten; falling back to menuconfig's Wi-Fi settings");
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
