// The words the room is described in (installation.cppm): which device is
// which (Address), what a light shows (a Look), when the lights may be on (a
// Gate), and the zones that tie them together. Plain data, like :types;
// sunsim::core evaluates them.

export module sunsim.ev:topology;

import std.compat;
import :types;

export namespace sunsim::ev {

// ---- Building blocks --------------------------------------------------------

// A check of the room failed. Not constexpr, so reaching it in a constant
// expression stops the compile, and GCC shows the call, message and all:
// `fail("a light is in no zone")`. (Reached at run time, it aborts.)
[[noreturn]] inline void fail(const char * /*why*/) noexcept
{
    std::abort();
}

// A value fixed in the installation, or a ProgramConfig field: a setting,
// which `config <name>` changes and NVS keeps. `value(config)` reads it.
template <class T> class Tunable {
public:
    constexpr Tunable(T value) noexcept : value_(value) {}                       // NOLINT: implicit by design
    constexpr Tunable(T ProgramConfig::*setting) noexcept : setting_(setting) {} // NOLINT: likewise

    [[nodiscard]] constexpr T operator()(const ProgramConfig &config) const noexcept
    {
        return setting_ ? config.*setting_ : value_;
    }

private:
    T                   value_{};
    T ProgramConfig::*setting_ = nullptr;
};

// A short list, sized in place, for the room's tables (std::inplace_vector,
// once libstdc++ has it).
template <class T, std::size_t N> class List {
public:
    constexpr List() noexcept = default;
    constexpr List(std::initializer_list<T> items) noexcept : size_(items.size())
    {
        if (items.size() > N) {
            fail("more items than the list holds");
        }
        std::ranges::copy(items, items_.begin());
    }

    [[nodiscard]] constexpr const T *begin() const noexcept { return items_.data(); }
    [[nodiscard]] constexpr const T *end() const noexcept { return items_.data() + size_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

private:
    std::array<T, N> items_{};
    std::size_t      size_ = 0;
};

// ---- Addresses: which device is which ---------------------------------------

// A device's IEEE (EUI-64) address, from its label: when it joins, it takes
// `role`.
struct Address {
    std::string_view role;         // one role: "WallA", "Work", any case
    uint64_t         ext_addr = 0; // 0: not known yet (claim it with `pair`)
};

// Each role's address; 0: none given.
using Addresses = PerRole<uint64_t>;

// `entries` per role: a name that isn't one role, a role listed twice, or
// one address given to two roles doesn't compile.
template <std::ranges::input_range Entries> [[nodiscard]] consteval Addresses addressesOf(const Entries &entries)
{
    Addresses     addresses{};
    PerRole<bool> listed{};
    for (const Address &entry : entries) {
        const auto role = parseRole(entry.role);
        if (!role) {
            fail("an address names no role");
        }
        if (listed[*role]) {
            fail("a role has two addresses");
        }
        if (entry.ext_addr != 0 && std::ranges::contains(addresses.items, entry.ext_addr)) {
            fail("an address is given to two roles");
        }
        listed[*role]    = true;
        addresses[*role] = entry.ext_addr;
    }
    return addresses;
}

// ---- Looks: what a light shows ----------------------------------------------

// The sun at the configured location: warm and dim at sunrise and sunset,
// brightest and coolest at solar noon, off at night. A group showing it is
// spread along a south wall, east to west in letter order: the sun direction
// and the amber glow follow the sun along it. Shaped by the sun's and the
// sky's settings (sunshift, amber, clouds, ...).
struct SunCurve {};

// Cool -> warm across the hours, easing in so it stays cool through the
// morning; cool before them, warm after. Full brightness.
struct WorkCurve {
    Tunable<DaySpan>  hours = &ProgramConfig::work;
    Tunable<uint16_t> cool  = &ProgramConfig::work_cool_mireds;
    Tunable<uint16_t> warm  = &ProgramConfig::work_warm_mireds;
};

using Look = std::variant<SunCurve, WorkCurve>;

// What a group's lights, or one light, show (in a zone).
struct Shows {
    std::string_view lights; // a group ("Wall") or one light ("Work"), any case
    Look             look;
};

// ---- Gates: when the lights may be on ---------------------------------------

struct Rule;

// "On", always.
struct Always {};

// "On" inside the hours on the days, fading in and out over `fade` at each
// edge; "off" outside.
struct Window {
    Tunable<DaySpan>              hours;
    Tunable<Weekdays>             days = kEveryDay;
    Tunable<std::chrono::minutes> fade = std::chrono::minutes{0};

    // Only its "on" (outside, the rules after it decide), or only its "off"
    // (inside, they do).
    [[nodiscard]] constexpr Rule opensOnly() const noexcept;
    [[nodiscard]] constexpr Rule closesOnly() const noexcept;
};

// Which of its answers a rule gives; the other case falls to the rules after it.
enum class Says : uint8_t { Both, OnOnly, OffOnly };

struct Rule {
    std::variant<Always, Window> source;
    Says                         says = Says::Both;

    constexpr Rule() noexcept = default;
    constexpr Rule(Always always) noexcept : source(always) {}                              // NOLINT: implicit by design
    constexpr Rule(Window window, Says says = Says::Both) noexcept : source(window), says(says) {} // NOLINT: likewise
};

constexpr Rule Window::opensOnly() const noexcept { return {*this, Says::OnOnly}; }
constexpr Rule Window::closesOnly() const noexcept { return {*this, Says::OffOnly}; }

// A rule that answers whatever the time, so nothing after it in a gate is
// ever asked: `Always`, and a `Window` that gives both answers. (Presence,
// once it's here, won't be: it has no answer while its sensor is unknown.)
[[nodiscard]] constexpr bool answersAlways(const Rule &rule) noexcept
{
    return std::holds_alternative<Always>(rule.source) || rule.says == Says::Both;
}

// The first rule with an answer decides; when none answers, the lights are off.
using Gate = List<Rule, 4>;

// ---- Zones: lights that share a mode, a gate and their buttons --------------

// A zone, by its place in the zones table.
struct ZoneId {
    uint8_t index = 0;

    // Spelled out, like DaySpan's: defaulted, GCC 15 crashes compiling a
    // file that imports this and compares ZoneIds.
    friend constexpr bool operator==(ZoneId a, ZoneId b) noexcept { return a.index == b.index; }
};

// What a button's presses choose for its zone; nullopt: that press does
// nothing.
struct Controls {
    std::string_view    by; // a button: "Button", any case
    std::optional<Mode> single = Mode::Daylight;
    std::optional<Mode> twice  = Mode::RedAlert; // a double press
    std::optional<Mode> hold   = Mode::Off;
};

// The lights the weather pulses (core::Overlays), each a group or a light:
// precipitation lights pulse a color while rain or snow falls, alert lights
// yellow, orange or red for an NWS advisory, watch or warning (an alert wins
// where they overlap). Color lights in the zone only.
struct Weather {
    List<std::string_view, 8> precip;
    List<std::string_view, 8> alerts;
};

// Lights that share a mode (chosen by their buttons, or the console) and a
// gate. Each light is in exactly one zone; it starts in daylight.
struct ZoneSpec {
    std::string_view  name;
    List<Shows, 8>    lights;
    Gate              gate  = {Always{}};
    List<Mode, 4>     modes = {Mode::Daylight, Mode::Off, Mode::RedAlert, Mode::Tuning}; // what it may run
    List<Controls, 2> controls{};
    std::optional<Weather> weather{}; // none: the weather never touches these lights

    [[nodiscard]] constexpr bool allows(Mode mode) const noexcept { return std::ranges::contains(modes, mode); }
};

// Every zone's weather lights.
struct WeatherLights {
    RoleSet precip{};
    RoleSet alerts{};
};

// The zones table, per role: checked when it's built.
struct Topology {
    PerRole<std::optional<ZoneId>>   zone_of{};     // a light's zone, or the zone a button controls
    PerRole<Look>                    look_of{};     // lights
    PerRole<std::optional<Controls>> controls_of{}; // buttons that control a zone
    WeatherLights                    weather{};
};

// `zones` per role. Doesn't compile if two zones share a name; a zone has no
// lights, no gate, a gate rule after one that always answers
// (answersAlways), or can't run daylight; a name in it isn't in the layout,
// gives a button a look or a light a second one; a light is in no zone; or
// a button is named twice, isn't a button, or picks a mode its zone can't run;
// or a weather light isn't a color light in its zone.
template <std::ranges::input_range Zones> [[nodiscard]] consteval Topology topologyOf(const Zones &zones)
{
    Topology      t{};
    PerRole<bool> shown{};
    uint8_t       index = 0;
    for (const ZoneSpec &zone : zones) {
        const ZoneId id{index++};
        for (const ZoneSpec &other : zones) {
            if (&other != &zone && other.name == zone.name) {
                fail("two zones share a name");
            }
        }
        if (zone.lights.empty()) {
            fail("a zone has no lights");
        }
        if (zone.gate.empty()) {
            fail("a zone has no gate");
        }
        for (const Rule *rule = zone.gate.begin(); rule + 1 < zone.gate.end(); ++rule) {
            if (answersAlways(*rule)) {
                fail("a gate rule follows one that always answers: it's never asked");
            }
        }
        if (!zone.allows(Mode::Daylight)) {
            fail("a zone can't run daylight (every zone starts in it)");
        }
        for (const Shows &s : zone.lights) {
            const auto selection = parseSelection(s.lights);
            if (!selection) {
                fail("a zone's lights name something the layout doesn't have");
            }
            for (const Role role : kRoles) {
                if (!selects(*selection, role)) {
                    continue;
                }
                if (!isLight(role)) {
                    fail("a zone gives a button a look");
                }
                if (shown[role]) {
                    fail("a light is in two zones, or shown twice");
                }
                shown[role]     = true;
                t.look_of[role] = s.look;
                t.zone_of[role] = id;
            }
        }
        for (const Controls &c : zone.controls) {
            const auto button = parseRole(c.by);
            if (!button || !isButton(*button)) {
                fail("a zone's controls name something that isn't a button");
            }
            if (t.controls_of[*button]) {
                fail("a button controls two zones, or one twice");
            }
            for (const std::optional<Mode> &mode : {c.single, c.twice, c.hold}) {
                if (mode && !zone.allows(*mode)) {
                    fail("a button picks a mode its zone can't run");
                }
            }
            t.controls_of[*button] = c;
            t.zone_of[*button]     = id;
        }
        if (zone.weather) {
            const auto add = [&](const auto &names, RoleSet &set) {
                for (const std::string_view name : names) {
                    const auto selection = parseSelection(name);
                    if (!selection) {
                        fail("a zone's weather lights name something the layout doesn't have");
                    }
                    for (const Role role : kRoles) {
                        if (selects(*selection, role)) {
                            if (t.zone_of[role] != id || !caps(role).color) {
                                fail("a weather light isn't a color light in its zone");
                            }
                            set.set(slot(role));
                        }
                    }
                }
            };
            add(zone.weather->precip, t.weather.precip);
            add(zone.weather->alerts, t.weather.alerts);
        }
    }
    for (const Role role : lights()) {
        if (!shown[role]) {
            fail("a light is in no zone");
        }
    }
    return t;
}

} // namespace sunsim::ev
