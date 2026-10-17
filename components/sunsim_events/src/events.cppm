// Every event in the system, in one variant. A module never calls another
// module: it posts one of these, and the bus delivers it to every module
// with an `on()` for it (EventBus::attach). See ARCHITECTURE.md §7 for who
// posts what.

export module sunsim.ev:events;

import std.compat;
import :types;

export namespace sunsim::ev {

// ---- Clock ----------------------------------------------------------------

struct Heartbeat {};             // raw 1 Hz timer; the Clock turns it into Tick
struct Tick {                    // 1 Hz, with the program's current time
    Instant     at{};
    TimeReading time{};
};
struct ProgramTick {             // every daylight update interval (60 s, 1 s in fast-day)
    TimeReading time{};
    Transition  transition{};    // ramp over the whole interval
};
struct SetFastDay {              // console `fastday`: the debug fast-day clock on or off
    bool                 on = false;
    std::chrono::seconds day_length{120}; // real seconds per simulated active window
};

// ---- Settings -------------------------------------------------------------

struct ConfigChanged {           // at boot from NVS, then after every change
    ProgramConfig config{};
    Peaks         peaks      = kDefaultPeaks;
    MinLevels     min_levels = kDefaultMinLevels;
};
struct SetConfig {               // console `config ...`
    ProgramConfig config{};
};
struct SetIntensity {            // console `intensity ...`
    Selection lights;            // non-bulbs in it are skipped
    uint8_t   percent = 100;
};
struct SetMinLevel {             // console `minlevel ...`, `tune save`
    Selection lights;            // non-bulbs in it are skipped
    uint8_t   level = 1;         // 1-254
};
struct CloudCoverUpdated {       // InternetSync, after each daily fetch
    CloudCover forecast{};
};
struct ConditionsUpdated {       // InternetSync, every 15 min; console `weather simulate`
    Conditions now{};
    Instant    at{};             // when fetched: it goes stale kWeatherFreshFor later
};
struct AlertsUpdated {           // InternetSync, every 15 min (none in force: count 0)
    Alerts  alerts{};
    Instant at{};
};
struct SkyChanged {              // SkyModel, whenever the sky now changes
    Sky sky{};
};
struct RolesLoaded {             // at boot from NVS
    DeviceRecords records{};
};
struct PersistRoles {            // DeviceRegistry -> SettingsStore
    DeviceRecords records{};
};

// ---- From the Zigbee stack ------------------------------------------------

struct NetworkUp {               // network formed, or rejoined after a reboot
    bool    rejoined = false;
    Instant at{};
};
struct DeviceAnnounced {
    uint64_t ext_addr   = 0;
    uint16_t short_addr = 0;
};
struct EndpointFound {           // reply to Discover; endpoint 0 = none found
    Role    role{};
    uint8_t endpoint = 0;
};
struct BindResult {
    Role role{Group::Button};
    bool ok   = false;
};
struct PingResult {
    Role role{};
    bool ok     = false;
    int  error  = 0;
    int  status = 0;
};
struct AckReceived {             // ZCL Default Response from a bulb
    uint16_t short_addr = 0;
    int8_t   rssi       = 0;
    uint8_t  status     = 0;
};

enum class Press : uint8_t {     // what each one does is ModeController's call
    Single,
    Double,
    Hold,
    Ambiguous, // On/Toggle seen only as an attribute change: single vs double by timing
};
enum class Source : uint8_t { Zigbee, Console };
struct ButtonPressed {
    Press   press  = Press::Single;
    Source  source = Source::Zigbee;
    Instant at{};
};

// ---- Console requests -----------------------------------------------------

struct PairRequested {
    Role role{};
};
struct PairCanceled {};
struct UnpairRequested {
    Selection roles;
};
using OpenTime = std::chrono::duration<uint8_t>; // what the stack takes: whole seconds, up to 254
inline constexpr OpenTime kDefaultOpenTime{180};  // pairing, console `open`, network up
struct OpenNetwork {
    OpenTime duration = kDefaultOpenTime;
};
struct FactoryReset {};
struct SyncRequested {};         // fetch from the internet now (clock, ...): console `sync`, fast-day switches
struct ModeRequested {           // console: `tune`, `tune off`
    Mode mode = Mode::Daylight;
};
struct TuneLights {              // console `tune <lights> <level> [kelvin]`: the tuning program's levels
    Selection               lights;
    uint8_t                 level = 1;
    std::optional<uint16_t> kelvin{}; // none: keep the color
};
struct TuneSave {};              // console `tune save`: the tuned levels become the minimums

// ---- DeviceRegistry -------------------------------------------------------

struct RoleAssigned {            // at boot, on pairing, and on any address change
    Role         role{};
    DeviceRecord record{};
};
struct RoleCleared {
    Role role{};
};
struct Discover {
    Role     role{};
    uint16_t short_addr = 0;
};
struct Bind {                    // button's On/Off output -> our receiver endpoint
    Role     role{Group::Button};
    uint64_t ext_addr   = 0;
    uint16_t short_addr = 0;
    uint8_t  endpoint   = 0;
};
struct Ping {
    Role     role{};
    uint16_t short_addr = 0;
};
struct ReachabilityChanged {     // a bulb stopped answering / answers again
    Role role{};
    bool reachable = true;
};
struct DevicesUpdated {          // 1 Hz summary for the status model
    PerRole<DeviceStatus> devices{};
    DeviceRecords         records{};
    std::optional<Role>   armed;
};

// ---- Mode, program, output ------------------------------------------------

struct ModeChanged {             // switch to another program
    Mode mode = Mode::Daylight;
};
struct RefreshLights {           // the running program re-applies now, with its quick transition
    std::optional<Role> role;    // nullopt = every light
};
struct LightChange {             // a bulb, what it's to show, and how quickly to get there
    Role       role{};
    LightState state{};
    Transition level_transition{};
    Transition color_transition{};
};
struct LightTarget : LightChange {};  // what a bulb should show, from the running program
struct OutputTarget : LightChange {}; // what it does show: Overlays' pick, for LightOutput
struct SetLight {                // one bulb's ZCL commands, sent in field order
    uint16_t               short_addr = 0;
    uint8_t                endpoint   = 0;
    std::optional<bool>    power;   // On goes first, Off last
    std::optional<uint8_t> level;
    std::optional<Color>   color;
    Transition             level_transition{};
    Transition             color_transition{};
};

// ---- Status ---------------------------------------------------------------

struct StatusChanged {           // shared, not copied: it's by far the largest payload,
    std::shared_ptr<const StatusSnapshot> snapshot; // and every Event is as big as its largest
};

// ---- The event type ------------------------------------------------------

using Payload = std::variant<Heartbeat, Tick, ProgramTick, SetFastDay,                                 //
                             ConfigChanged, SetConfig, SetIntensity, SetMinLevel, CloudCoverUpdated,  //
                             ConditionsUpdated, AlertsUpdated, SkyChanged,                            //
                             RolesLoaded, PersistRoles,                                               //
                             NetworkUp, DeviceAnnounced, EndpointFound, BindResult, PingResult,       //
                             AckReceived, ButtonPressed,                                              //
                             PairRequested, PairCanceled, UnpairRequested, OpenNetwork, FactoryReset, //
                             SyncRequested, ModeRequested, TuneLights, TuneSave,                      //
                             RoleAssigned, RoleCleared, Discover, Bind, Ping, ReachabilityChanged,    //
                             DevicesUpdated,                                                          //
                             ModeChanged, RefreshLights, LightTarget, OutputTarget, SetLight,        //
                             StatusChanged>;

namespace detail {
template <class T, class... Ts> constexpr std::size_t indexIn(const std::variant<Ts...> *) noexcept
{
    constexpr std::array matches{std::is_same_v<T, Ts>...};
    return static_cast<std::size_t>(std::ranges::find(matches, true) - matches.begin());
}
} // namespace detail

// The event-type id of payload T (its index in Payload): what listeners key on.
template <class T>
inline constexpr std::size_t kTypeId = detail::indexIn<T>(static_cast<const Payload *>(nullptr));

// A type that travels on the bus: one of Payload's alternatives.
template <class T>
concept EventPayload = kTypeId<T> < std::variant_size_v<Payload>;

namespace detail {
// T's unqualified name, read off the compiler's name for this function:
// "... [with T = sunsim::ev::Tick@sunsim.ev; ...]" -> "Tick".
template <class T> consteval std::string_view typeName() noexcept
{
    std::string_view name = __PRETTY_FUNCTION__;
    name.remove_prefix(name.find("T = ") + 4);
    name = name.substr(0, name.find_first_of(";]@"));
    if (const auto scope = name.rfind("::"); scope != std::string_view::npos) {
        name.remove_prefix(scope + 2);
    }
    return name;
}
} // namespace detail

// Every payload's name, by type id: for tracing.
inline constexpr auto kEventNames = []<std::size_t... I>(std::index_sequence<I...>) {
    return std::array{detail::typeName<std::variant_alternative_t<I, Payload>>()...};
}(std::make_index_sequence<std::variant_size_v<Payload>>{});
static_assert(kEventNames[kTypeId<Tick>] == "Tick" && kEventNames[kTypeId<StatusChanged>] == "StatusChanged");

struct Event {
    Payload data;

    template <class T>
        requires EventPayload<std::remove_cvref_t<T>>
    Event(T &&payload) : data(std::forward<T>(payload)) // NOLINT: implicit by design - post(ev::Tick{...})
    {
    }

    [[nodiscard]] std::size_t type() const noexcept { return data.index(); }
    [[nodiscard]] std::string_view name() const noexcept { return kEventNames[data.index()]; }
    template <EventPayload T> [[nodiscard]] const T &as() const { return *std::get_if<T>(&data); }

    Event(const Event &);
    Event(Event &&) noexcept;
    Event &operator=(const Event &);
    Event &operator=(Event &&) noexcept;
    ~Event();
};

// Not inline: sunsim.ev's object file has the one copy. (Implicit, GCC 15
// left importers that move or destroy an Event linking to std::variant
// internals nobody emitted - as with Timer's destructor in :flow.)
Event::Event(const Event &)                = default;
Event::Event(Event &&) noexcept            = default;
Event &Event::operator=(const Event &)     = default;
Event &Event::operator=(Event &&) noexcept = default;
Event::~Event()                            = default;

// What every module gets instead of a reference to the bus or to another
// module. Thread-safe: may be called from any task.
using Post = std::function<void(Event)>;

} // namespace sunsim::ev
