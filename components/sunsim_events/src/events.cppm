// Every event in the system, in one variant. A module never calls another
// module: it posts one of these, and the wiring (wireCore / main's
// wirePlatform) routes it. See EVENT_DESIGN.md §5 for who posts what.

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

// ---- Settings -------------------------------------------------------------

struct ConfigChanged {           // at boot from NVS, then after every change
    ProgramConfig config{};
    Peaks         peaks = kDefaultPeaks;
};
struct SetConfig {               // console `config ...`
    ProgramConfig config{};
};
struct SetIntensity {            // console `intensity ...`
    std::optional<Role> role;    // nullopt = every light
    uint8_t             percent = 100;
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
    Role    role     = Role::WallA;
    uint8_t endpoint = 0;
};
struct BindResult {
    Role role = Role::Button;
    bool ok   = false;
};
struct PingResult {
    Role role   = Role::WallA;
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
    Role role = Role::WallA;
};
struct PairCancelled {};
struct UnpairRequested {
    std::optional<Role> role;    // nullopt = all
};
using OpenTime = std::chrono::duration<uint8_t>; // what the stack takes: whole seconds, up to 254
inline constexpr OpenTime kDefaultOpenTime{180};  // pairing, console `open`, network up
struct OpenNetwork {
    OpenTime duration = kDefaultOpenTime;
};
struct FactoryReset {};

// ---- DeviceRegistry -------------------------------------------------------

struct RoleAssigned {            // at boot, on pairing, and on any address change
    Role         role = Role::WallA;
    DeviceRecord record{};
};
struct RoleCleared {
    Role role = Role::WallA;
};
struct Discover {
    Role     role       = Role::WallA;
    uint16_t short_addr = 0;
};
struct Bind {                    // button's On/Off output -> our receiver endpoint
    Role     role       = Role::Button;
    uint64_t ext_addr   = 0;
    uint16_t short_addr = 0;
    uint8_t  endpoint   = 0;
};
struct Ping {
    Role     role       = Role::WallA;
    uint16_t short_addr = 0;
};
struct ReachabilityChanged {     // a bulb stopped answering / answers again
    Role role      = Role::WallA;
    bool reachable = true;
};
struct DevicesUpdated {          // 1 Hz summary for the status model
    std::array<DeviceStatus, kRoleCount> devices{};
    DeviceRecords                        records{};
    std::optional<Role>                  armed;
};

// ---- Mode, program, output ------------------------------------------------

struct ModeChanged {             // switch to another program
    Mode mode = Mode::Daylight;
};
struct RefreshLights {           // the running program re-applies now, with its quick transition
    std::optional<Role> role;    // nullopt = every light
};
struct LightTarget {             // what a bulb should show, from the running program
    Role       role = Role::WallA;
    LightState state{};
    Transition level_transition{};
    Transition color_transition{};
};
struct SetLight {                // one bulb's ZCL commands, sent in field order
    uint16_t                                  short_addr = 0;
    uint8_t                                   endpoint   = 0;
    std::optional<bool>                       power;   // On goes first, Off last
    std::optional<uint8_t>                    level;
    std::optional<uint16_t>                   mireds;
    std::optional<std::pair<float, float>>    xy;
    std::optional<std::pair<uint8_t, uint8_t>> hue_sat;
    Transition                                level_transition{};
    Transition                                color_transition{};
};

// ---- Status ---------------------------------------------------------------

struct StatusChanged {           // shared, not copied: it's by far the largest payload,
    std::shared_ptr<const StatusSnapshot> snapshot; // and every Event is as big as its largest
};

// ---- The event type ------------------------------------------------------

using Payload = std::variant<Heartbeat, Tick, ProgramTick,                                             //
                             ConfigChanged, SetConfig, SetIntensity, RolesLoaded, PersistRoles,       //
                             NetworkUp, DeviceAnnounced, EndpointFound, BindResult, PingResult,       //
                             AckReceived, ButtonPressed,                                              //
                             PairRequested, PairCancelled, UnpairRequested, OpenNetwork, FactoryReset, //
                             RoleAssigned, RoleCleared, Discover, Bind, Ping, ReachabilityChanged,    //
                             DevicesUpdated,                                                          //
                             ModeChanged, RefreshLights, LightTarget, SetLight,                      //
                             StatusChanged>;

// Names for tracing, in Payload order.
inline constexpr const char *kEventNames[] = {
    "Heartbeat", "Tick", "ProgramTick",
    "ConfigChanged", "SetConfig", "SetIntensity", "RolesLoaded", "PersistRoles",
    "NetworkUp", "DeviceAnnounced", "EndpointFound", "BindResult", "PingResult",
    "AckReceived", "ButtonPressed",
    "PairRequested", "PairCancelled", "UnpairRequested", "OpenNetwork", "FactoryReset",
    "RoleAssigned", "RoleCleared", "Discover", "Bind", "Ping", "ReachabilityChanged", "DevicesUpdated",
    "ModeChanged", "RefreshLights", "LightTarget", "SetLight",
    "StatusChanged",
};
static_assert(std::size(kEventNames) == std::variant_size_v<Payload>, "kEventNames out of sync with Payload");

namespace detail {
template <class T, class... Ts>
constexpr std::size_t indexIn(const std::variant<Ts...> *) noexcept
{
    constexpr bool        matches[] = {std::is_same_v<T, Ts>...};
    std::size_t           i         = 0;
    for (; i < sizeof...(Ts) && !matches[i]; ++i) {
    }
    return i;
}
} // namespace detail

// The event-type id of payload T (its index in Payload): what listeners key on.
template <class T>
inline constexpr std::size_t kTypeId = detail::indexIn<T>(static_cast<const Payload *>(nullptr));

struct Event {
    Payload data;

    template <class T, class = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Event>>>
    Event(T &&payload) : data(std::forward<T>(payload)) // NOLINT: implicit by design - post(ev::Tick{...})
    {
        static_assert(kTypeId<std::decay_t<T>> < std::variant_size_v<Payload>, "not an event payload type");
    }

    [[nodiscard]] std::size_t type() const noexcept { return data.index(); }
    [[nodiscard]] const char *name() const noexcept { return kEventNames[data.index()]; }
    template <class T> [[nodiscard]] const T &as() const { return *std::get_if<T>(&data); }
};

// What every module gets instead of a reference to the bus or to another
// module. Thread-safe: may be called from any task.
using Post = std::function<void(Event)>;

} // namespace sunsim::ev
