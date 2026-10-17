// The words the room is described in (installation.cppm): what a light shows
// (a Look) and when the lights may be on (a Gate). Plain data, like :types;
// sunsim::core evaluates them.

export module sunsim.ev:topology;

import std.compat;
import :types;

export namespace sunsim::ev {

// ---- Building blocks --------------------------------------------------------

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
            tooManyItems();
        }
        std::ranges::copy(items, items_.begin());
    }

    [[nodiscard]] constexpr const T *begin() const noexcept { return items_.data(); }
    [[nodiscard]] constexpr const T *end() const noexcept { return items_.data() + size_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

private:
    // Not constexpr: reached in a constant table, it stops the compile.
    static void tooManyItems() noexcept { std::abort(); }

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

namespace detail {
// Not constexpr: reached while building kAddressOf, they stop the compile,
// and the error names them.
inline void anAddressNamesNoRole() noexcept {}
inline void aRoleHasTwoAddresses() noexcept {}
inline void anAddressIsGivenTwice() noexcept {}
} // namespace detail

// `entries` per role: a name that isn't one role, a role listed twice, or
// one address given to two roles doesn't compile.
template <std::ranges::input_range Entries> [[nodiscard]] consteval Addresses addressesOf(const Entries &entries)
{
    Addresses     addresses{};
    PerRole<bool> listed{};
    for (const Address &entry : entries) {
        const auto role = parseRole(entry.role);
        if (!role) {
            detail::anAddressNamesNoRole();
        }
        if (listed[*role]) {
            detail::aRoleHasTwoAddresses();
        }
        if (entry.ext_addr != 0 && std::ranges::contains(addresses.items, entry.ext_addr)) {
            detail::anAddressIsGivenTwice();
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

// What a group's lights, or one light, show.
struct Shows {
    std::string_view lights; // a group ("Wall") or one light ("Work"), any case
    Look             look;
};

namespace detail {
// Not constexpr: reached while building kLookOf, they stop the compile, and
// the error names them.
inline void showsNamesNoLight() noexcept {}
inline void showsGivesALookToAButton() noexcept {}
inline void showsGivesALightTwoLooks() noexcept {}
inline void aLightShowsNothing() noexcept {}
} // namespace detail

// Each light's look, from `shows`: a name the layout doesn't have, a light
// given two looks or none, or a look on a button doesn't compile.
template <std::ranges::input_range ShowsTable>
[[nodiscard]] consteval PerRole<Look> looksOf(const ShowsTable &shows)
{
    PerRole<Look>  looks{};
    PerRole<bool>  given{};
    for (const Shows &s : shows) {
        const auto selection = parseSelection(s.lights);
        if (!selection) {
            detail::showsNamesNoLight();
        }
        for (const Role role : kRoles) {
            if (!selects(*selection, role)) {
                continue;
            }
            if (!isBulb(role)) {
                detail::showsGivesALookToAButton();
            }
            if (given[role]) {
                detail::showsGivesALightTwoLooks();
            }
            looks[role] = s.look;
            given[role] = true;
        }
    }
    for (const Role role : bulbs()) {
        if (!given[role]) {
            detail::aLightShowsNothing();
        }
    }
    return looks;
}

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

// The first rule with an answer decides; when none answers, the lights are off.
using Gate = List<Rule, 4>;

} // namespace sunsim::ev
