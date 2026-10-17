// The program settings people change (console `config <name> <value>`) and
// the device keeps (NVS, a key per field), as one table: the console's
// parsing, printing and help and the store's keys all come from it, so a
// new setting is a ProgramConfig field and a line here.

export module sunsim.ev:settings;

import std.compat;
import :types;

export namespace sunsim::ev {

namespace setting {

// What kind of value a setting holds: how it's typed, shown and stored.
struct OnOff { // on | off; stored as a u8
    bool ProgramConfig::*field;
    const char           *key;
};
struct Number { // a whole number in a range; stored as a u8
    uint8_t ProgramConfig::*field;
    uint8_t                 min;
    uint8_t                 max;
    std::string_view        unit; // shown after the value: "%", " deg"
    const char             *key;

    [[nodiscard]] constexpr bool accepts(long v) const noexcept { return v >= min && v <= max; }
};
struct Minutes { // a duration; stored as whole minutes, a u16
    std::chrono::minutes ProgramConfig::*field;
    std::chrono::minutes                 max;
    const char                          *key;

    [[nodiscard]] constexpr bool accepts(std::chrono::minutes v) const noexcept
    {
        return v >= std::chrono::minutes{0} && v <= max;
    }
};
struct Span { // HH:MM-HH:MM; each end stored as minutes after midnight, a u16
    DaySpan ProgramConfig::*field;
    const char             *start_key;
    const char             *end_key;
};
struct Days { // mon-fri | all | mon,wed,...; stored as Weekdays' bits, a u8
    Weekdays ProgramConfig::*field;
    const char              *key;
};

using Kind = std::variant<OnOff, Number, Minutes, Span, Days>;

} // namespace setting

struct Setting {
    std::string_view name; // `config <name> <value>`
    setting::Kind    kind;
    std::string_view help;
};

inline constexpr std::array kSettings = [] {
    using PC = ProgramConfig;
    return std::array{
        Setting{"hours", setting::Span{&PC::active, "act.start", "act.end"}, "when the lights may be on"},
        Setting{"days", setting::Days{&PC::active_days, "act.days"}, "the days they may be on"},
        Setting{"fade", setting::Minutes{&PC::fade, std::chrono::minutes{180}, "act.fade"}, "fade in and out at each edge of the hours"},
        Setting{"workhours", setting::Span{&PC::work, "work.start", "work.end"},
                "the work light: cool -> warm across these"},
        Setting{"sunshift", setting::Number{&PC::sun_shift_percent, 0, 100, "%", "sun.shift"},
                "the wall dims away from the sun by up to this (0: off)"},
        Setting{"amber", setting::OnOff{&PC::amber_enabled, "sun.amber"}, "an amber glow while the sun is low"},
        Setting{"ambersweep", setting::Number{&PC::amber_sweep_deg, 0, kMaxAmberSweepDeg, " deg", "sun.sweep"},
                "the glow follows the sun: the far end lags this much (0: all alike)"},
        Setting{"clouds", setting::OnOff{&PC::clouds_enabled, "sky.on"}, "cloud cover shapes the wall (off: always clear)"},
        Setting{"clouddim", setting::Number{&PC::cloud_dim_percent, 0, 100, "%", "sky.dim"},
                "full overcast: up to this much dimmer"},
        Setting{"cloudcool", setting::Number{&PC::cloud_cool_percent, 0, 100, "%", "sky.cool"},
                "... and this far to the cool end"},
        Setting{"weather", setting::OnOff{&PC::weather_enabled, "wx.on"}, "the weather lights (each zone declares which)"},
        Setting{"syncwindow", setting::Span{&PC::sync, "sync.start", "sync.end"}, "the daily clock and forecast sync"},
    };
}();

[[nodiscard]] constexpr const Setting *findSetting(std::string_view name) noexcept
{
    const auto found = std::ranges::find(kSettings, name, &Setting::name);
    return found == kSettings.end() ? nullptr : &*found;
}

// Sets `setting` in `config` from what was typed, or says why not.
[[nodiscard]] std::expected<void, std::string> set(const Setting &setting, ProgramConfig &config,
                                                   std::string_view text);

// Its value in `config`, as set() takes it (plus a unit).
[[nodiscard]] std::string show(const Setting &setting, const ProgramConfig &config);

// What set() takes: "on|off", "0-100", "HH:MM-HH:MM", ...
[[nodiscard]] std::string syntax(const Setting &setting);

// Every NVS key the table uses.
[[nodiscard]] constexpr std::vector<std::string_view> settingKeys()
{
    std::vector<std::string_view> keys;
    for (const Setting &s : kSettings) {
        std::visit(
            [&](const auto &k) {
                if constexpr (requires { k.key; }) {
                    keys.emplace_back(k.key);
                } else {
                    keys.emplace_back(k.start_key);
                    keys.emplace_back(k.end_key);
                }
            },
            s.kind);
    }
    return keys;
}

namespace detail {
consteval bool settingsAreDistinct()
{
    auto keys = settingKeys();
    std::ranges::sort(keys);
    std::vector<std::string_view> names;
    for (const Setting &s : kSettings) {
        names.push_back(s.name);
    }
    std::ranges::sort(names);
    return std::ranges::adjacent_find(keys) == keys.end() && std::ranges::adjacent_find(names) == names.end() &&
           std::ranges::all_of(keys, [](std::string_view k) { return k.size() <= 15; }); // NVS's limit
}
static_assert(settingsAreDistinct(), "settings need distinct names and distinct NVS keys of at most 15 chars");
} // namespace detail

} // namespace sunsim::ev

namespace sunsim::ev {

namespace {
constexpr std::array<std::string_view, 7> kDayNames = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

// A whole number, all of `text`.
std::optional<long> wholeNumber(std::string_view text) noexcept
{
    long value = 0;
    const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
    return err == std::errc{} && end == text.data() + text.size() ? std::optional{value} : std::nullopt;
}

// "HH:MM" as a time after midnight (24:00 allowed).
std::optional<std::chrono::minutes> clockTime(std::string_view text) noexcept
{
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || text.size() - colon != 3) {
        return std::nullopt;
    }
    const auto h = wholeNumber(text.substr(0, colon));
    const auto m = wholeNumber(text.substr(colon + 1));
    if (!h || !m || *h < 0 || *m < 0 || *m > 59) {
        return std::nullopt;
    }
    return std::chrono::hours{*h} + std::chrono::minutes{*m};
}

std::string clockText(std::chrono::minutes t)
{
    return std::format("{:02}:{:02}", std::chrono::floor<std::chrono::hours>(t).count(), (t % std::chrono::hours{1}).count());
}

// Monday first, as they're typed and shown.
constexpr std::array kWeek = {std::chrono::Monday, std::chrono::Tuesday,  std::chrono::Wednesday, std::chrono::Thursday,
                              std::chrono::Friday, std::chrono::Saturday, std::chrono::Sunday};

std::string_view dayName(std::chrono::weekday day) noexcept
{
    return kDayNames[day.c_encoding()];
}

std::optional<Weekdays> parseDays(std::string_view text) noexcept
{
    if (text == "mon-fri") {
        return kMondayToFriday;
    }
    if (text == "all") {
        return kEveryDay;
    }
    Weekdays days;
    for (;;) {
        const auto comma = text.find(',');
        const auto day   = std::ranges::find(kWeek, text.substr(0, comma), dayName);
        if (day == kWeek.end()) {
            return std::nullopt;
        }
        days.insert(*day);
        if (comma == std::string_view::npos) {
            return days;
        }
        text.remove_prefix(comma + 1);
    }
}

std::string daysText(Weekdays days)
{
    if (days == kMondayToFriday) {
        return "mon-fri";
    }
    if (days == kEveryDay) {
        return "all";
    }
    std::string text;
    for (const std::chrono::weekday day : kWeek) {
        if (days.contains(day)) {
            text += text.empty() ? "" : ",";
            text += dayName(day);
        }
    }
    return text.empty() ? "none" : text;
}

// The text set() takes for each kind, and its value.
struct Syntax {
    std::string operator()(const setting::OnOff &) const { return "on|off"; }
    std::string operator()(const setting::Number &k) const { return std::format("{}-{}", k.min, k.max); }
    std::string operator()(const setting::Minutes &k) const { return std::format("0-{} (minutes)", k.max.count()); }
    std::string operator()(const setting::Span &) const { return "HH:MM-HH:MM"; }
    std::string operator()(const setting::Days &) const { return "mon-fri|all|mon,tue,..."; }
};
} // namespace

std::expected<void, std::string> set(const Setting &setting, ProgramConfig &config, std::string_view text)
{
    const auto expected = [&] {
        return std::unexpected{std::format("{} takes {}", setting.name, syntax(setting))};
    };
    struct Set {
        ProgramConfig   &c;
        std::string_view text;

        bool operator()(const setting::OnOff &k) const
        {
            if (text != "on" && text != "off") {
                return false;
            }
            c.*k.field = text == "on";
            return true;
        }
        bool operator()(const setting::Number &k) const
        {
            const auto v = wholeNumber(text);
            if (!v || !k.accepts(*v)) {
                return false;
            }
            c.*k.field = static_cast<uint8_t>(*v);
            return true;
        }
        bool operator()(const setting::Minutes &k) const
        {
            const auto v = wholeNumber(text);
            if (!v || !k.accepts(std::chrono::minutes{*v})) {
                return false;
            }
            c.*k.field = std::chrono::minutes{*v};
            return true;
        }
        bool operator()(const setting::Span &k) const
        {
            const auto dash  = text.find('-');
            const auto start = clockTime(text.substr(0, dash));
            const auto end   = dash == std::string_view::npos ? std::nullopt : clockTime(text.substr(dash + 1));
            if (!start || !end || !DaySpan{*start, *end}.valid()) {
                return false;
            }
            c.*k.field = {*start, *end};
            return true;
        }
        bool operator()(const setting::Days &k) const
        {
            const auto days = parseDays(text);
            if (!days || days->bits == 0) {
                return false;
            }
            c.*k.field = *days;
            return true;
        }
    };
    if (!std::visit(Set{config, text}, setting.kind)) {
        return expected();
    }
    return {};
}

std::string show(const Setting &setting, const ProgramConfig &c)
{
    struct Show {
        const ProgramConfig &c;

        std::string operator()(const setting::OnOff &k) const { return c.*k.field ? "on" : "off"; }
        std::string operator()(const setting::Number &k) const { return std::format("{}{}", c.*k.field, k.unit); }
        std::string operator()(const setting::Minutes &k) const { return std::format("{} min", (c.*k.field).count()); }
        std::string operator()(const setting::Span &k) const
        {
            return clockText((c.*k.field).start) + "-" + clockText((c.*k.field).end);
        }
        std::string operator()(const setting::Days &k) const { return daysText(c.*k.field); }
    };
    return std::visit(Show{c}, setting.kind);
}

std::string syntax(const Setting &setting)
{
    return std::visit(Syntax{}, setting.kind);
}

} // namespace sunsim::ev
