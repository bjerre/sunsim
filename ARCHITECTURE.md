# sunsim — architecture

How the firmware is put together: the modules, the events between them,
the threads, and what happens at boot and on a button press. The domain
side (devices and roles, the button, the daylight math) is in
[DESIGN.md](DESIGN.md); building and using it in [README.md](README.md).

## 1. The rule

**Producers post events; one app task processes them.** A module never
calls another module and never takes a lock: it handles the events it's
wired to and posts new ones. Everything that touches ESP-IDF lives in
`sunsim::platform`; everything else is plain C++ in `sunsim::core` and runs
unchanged in the host tests.

This keeps every piece of application state on one thread (the app task
holds the Zigbee lock while it processes, so handlers may call the stack),
gives each module one job, and turns "what happens when..." into a chain of
events you can trace (`EventBus` logs every event at DEBUG, tag `EV`).

## 2. Layers

Each component is one C++ module (`import sunsim.ev;` ...), and ESP-IDF's
component `REQUIRES` enforce the layering: a `core` file that includes
`esp_zigbee.h` or imports `sunsim.platform` doesn't build.

```mermaid
flowchart TB
    subgraph app["main/ — namespace sunsim"]
        App["App: owns the bus and every module,<br/>wires the platform edges, runs the app task"]
    end
    subgraph platform["components/sunsim_platform — module sunsim.platform"]
        ZA[ZigbeeAdapter]
        CK["Clock<br/>(+ FastDayClock)"]
        IS["InternetSync<br/>(+ WifiSession)"]
        ST["SettingsStore (NVS)"]
        CO[Console]
        DI[StatusDisplay]
    end
    subgraph core["components/sunsim_core — module sunsim.core (pure C++, host-tested)"]
        CM["CoreModules + wireCore()"]
        DR[DeviceRegistry]
        MC[ModeController]
        PH["ProgramHost<br/>Daylight / Off / RedAlert / Tuning"]
        OV["Overlays<br/>(weather lights)"]
        LO[LightOutput]
        SM[StatusModel]
        MA["math: daylight, solar, sky, day_info"]
    end
    subgraph weather["components/sunsim_weather — module sunsim.weather (pure C++, host-tested)"]
        WR["JsonScanner, readers for<br/>Open-Meteo and NWS replies"]
    end
    subgraph events["components/sunsim_events — module sunsim.ev"]
        EV["events + value types<br/>EventBus, Flow / Timer / Inbox"]
    end
    IDF[("ESP-IDF: esp-zigbee, NVS, esp_timer,<br/>console, esp_lvgl_port, Wi-Fi/SNTP")]

    app --> platform & core
    platform --> weather --> events
    platform --> events
    core --> events
    platform --> IDF
```

| Namespace / module | Holds | Imports |
|---|---|---|
| `sunsim::ev` (`sunsim.ev`) | every event, the `Event` variant, `Post`, `EventBus`; coroutine flows (`Flow`, `Timer`, `Inbox`); value types (`Role` and the layout, `LightState`, `ProgramConfig`, `StatusSnapshot`, ...); the settings table (`kSettings`: each setting's name, kind, NVS key and help, with `set()` / `show()`) | `std` |
| `sunsim::core` (`sunsim.core`) | `DeviceRegistry`, `ModeController`, `SkyModel`, `ProgramHost` and the programs, `Overlays`, `LightOutput`, `StatusModel`, the daylight/solar/sky math; `CoreModules` and `wireCore()` | `ev` |
| `sunsim::weather` (`sunsim.weather`) | `JsonScanner` (streaming), the Open-Meteo and NWS reply readers, WMO codes and NWS alert tiers | `ev` |
| `sunsim::platform` (`sunsim.platform`) | `ZigbeeAdapter`, `Clock`, `InternetSync`, `SettingsStore`, the console, the status display, `StatusSlot` | `ev`, `weather`, ESP-IDF |
| `sunsim` (`main/main.cpp`) | `App`, `app_main` | all |

`core` and `platform` never import each other; anything both need lives in
`ev`. File-private helpers sit in anonymous namespaces, exported internals
in `detail`.

## 3. Classes

### Value types (`sunsim::ev`, `types.cppm`)

The installation is data: `kLayout` says how many devices each group has,
and the roles, their names (`WallA`-`WallE`, `Work`, `Button`), their slots
and `PerRole<T>` are all derived from it at compile time.

```mermaid
classDiagram
    direction LR
    class kLayout {
        <<constant>>
        Wall: 5
        Work: 1
        Button: 1
    }
    class GroupSpec {
        Group group
        string_view name
        uint8 count
    }
    class Group {
        <<enumeration>>
        Wall
        Work
        Button
    }
    class Role {
        Group group
        uint8 index
        roleName(Role) string_view
        parseRole(name) optional~Role~
        slot(Role) size_t
    }
    class PerRole~T~ {
        one T per role, in slot order
        indexed by Role: perRole[role]
    }
    class Selection {
        <<variant>>
        AllRoles
        Group
        Role
    }
    class DeviceRecord {
        bool valid
        uint64 ext_addr
        uint16 short_addr
        uint8 endpoint
    }
    class LightState {
        bool on
        uint8 level
        Color color : variant~Mireds, Xy, HueSat~
    }
    class ProgramConfig {
        double latitude_deg, longitude_deg
        mireds, levels
        sun_shift_percent
        amber_enabled, golden_elevation_deg
        amber_kelvin, amber_sweep_deg
        DaySpan active, minutes fade, Weekdays days
        DaySpan work, work mireds
        DaySpan sync
    }
    class StatusSnapshot {
        TimeReading time
        DayInfo day
        Mode mode
        PerRole~DeviceStatus~ devices
        DeviceRecords records
        optional~Role~ armed
        ProgramConfig config
        Peaks peaks
        MinLevels min_levels
        optional~uint8~ cloud_percent
    }
    class CloudCover {
        int16 year, yday
        uint8 percent[48]
        48 local hours from 00:00, 0-100 or kNoData
    }

    kLayout "1" *-- "3" GroupSpec
    GroupSpec --> Group
    Role --> Group
    Role ..> kLayout : names, slots and counts derived from
    PerRole~T~ ..> Role : indexed by
    Selection ..> Role
    StatusSnapshot *-- ProgramConfig
    StatusSnapshot ..> PerRole~T~ : devices, records, peaks
```

`DeviceRecords` is `PerRole<DeviceRecord>`; `Peaks` (percent) and
`MinLevels` (raw level) are `PerRole<uint8_t>`. `CloudCover` is the hourly
forecast, fetched by `InternetSync` each sync.
A wall light's place on the wall is its index: light `i` of `n` sits at
`x = 1 - 2i/(n-1)`, east (+1) to west (-1).

### Events and flows (`sunsim::ev`, `events.cppm`, `bus.cppm`, `flow.cppm`)

```mermaid
classDiagram
    direction LR
    class Event {
        Payload data
        Event(EventPayload auto payload)
        type() size_t
        name() string
        as~T~() const T&
    }
    class Payload {
        <<variant>>
        Heartbeat, Tick, ProgramTick
        ConfigChanged, ..., LightTarget, SetLight
        StatusChanged
    }
    class Post {
        <<function>>
        void(Event)
    }
    class EventBus {
        -vector~Event~ pending_
        -vector~Event~ batch_
        -listeners_ per payload type
        +on~T~(listener)
        +post() Post
        +wait()
        +process() bool
        +drain()
    }
    class Flow {
        <<coroutine>>
        starts at once
        canceled when reassigned or destroyed
    }
    class Timer {
        +now() Instant
        +until(Instant) awaitable
        +after(duration) awaitable
        +tick(Instant)
    }
    class Inbox~T~ {
        +next(filter, deadline) awaitable optional~T~
        +deliver(T, at)
    }

    Event *-- Payload
    EventBus ..> Event : queues, delivers
    EventBus *-- Post : enqueue
    Inbox~T~ --> Timer : deadlines from
    Flow ..> Timer : co_await
    Flow ..> Inbox~T~ : co_await
```

An event's type id is its index in `Payload` (`kTypeId<T>`), so there's no
separate enum to keep in step, and `EventPayload` is the concept "one of
`Payload`'s alternatives". Every module gets a `Post` at construction:
never the bus, never another module.

### Core (`sunsim::core`)

```mermaid
classDiagram
    direction TB
    class CoreModules {
        ModeController mode
        DeviceRegistry registry
        ProgramHost programs
        LightOutput output
        StatusModel status
    }
    class DeviceRegistry {
        -DeviceRecords records_
        -PerRole~Liveness~ liveness_
        -optional~Role~ armed_
        -PerRole~Flow~ discovery_
        -PerRole~Flow~ keep_alive_
        +manifestOf(entries) Manifest$
        +on(RolesLoaded / NetworkUp / DeviceAnnounced)
        +on(EndpointFound / PingResult / AckReceived)
        +on(PairRequested / UnpairRequested / ...)
        +on(Tick)
        -findEndpoint(Role) Flow
        -keepAlive(Role) Flow
    }
    class ModeController {
        -Inbox~ButtonPressed~ presses_
        -Flow flow_
        +on(ButtonPressed)
        +on(Tick)
        -run() Flow
    }
    class ProgramHost {
        -tuple~Daylight, Off, RedAlert, Tuning~ programs_ : in Mode order
        -Mode mode_
        -optional~TimeReading~ time_
        +on(ModeChanged / Tick / ProgramTick / RefreshLights / NetworkUp)
        +on(ConfigChanged / SkyChanged)
        +on(TuneLights / TuneSave)
        -running(f) / toEvery(e) / toRunning(e)
    }
    class Program {
        <<base>>
        #target(role, LightState, transitions)
        #forEachBulb(role, f)$
    }
    class DaylightProgram {
        -DaylightSchedule schedule_
        -Peaks peaks_
        -MinLevels min_levels_
        -uint8 cloud_percent_
        +on(SkyChanged)
        -outputLevel(role, level) uint8
    }
    class TuningProgram {
        -MinLevels min_levels_
        -PerRole~optional~ tuned_
        -double kelvin_
        +on(TuneLights)
        +takeTuned()
    }
    class OffProgram
    class RedAlertProgram {
        -bool bright_
    }
    class DaylightSchedule {
        +wallState(time, utc_offset, wall_position, cloud) LightState
        +workState(time) LightState
        +inActiveWindow(time) bool
    }
    class solar {
        <<functions>>
        computeSunTimes()
        computeSunPosition()
        computeDayInfo()
        cloudCover(forecast, time)
    }
    class SkyModel {
        -CloudCover forecast_
        -optional~ConditionsUpdated~ conditions_
        -optional~AlertsUpdated~ alerts_
        -optional~Sky~ posted_
        +on(Tick / CloudCoverUpdated / ConditionsUpdated / AlertsUpdated)
        -judge() Sky
    }
    class Overlays {
        -Sky sky_
        -PerRole~Light~ lights_ : base target, pulse
        +on(LightTarget)
        +on(Tick / SkyChanged)
        +on(ConfigChanged / ModeChanged)
        -reconcile()
    }
    class LightOutput {
        -PerRole~Bulb~ bulbs_
        +on(RoleAssigned / RoleCleared)
        +on(ReachabilityChanged)
        +on(OutputTarget)
    }
    class StatusModel {
        -StatusSnapshot snap_
        +on(ConfigChanged / ModeChanged / DevicesUpdated / Tick / SkyChanged)
    }

    CoreModules *-- DeviceRegistry
    CoreModules *-- ModeController
    CoreModules *-- SkyModel
    CoreModules *-- ProgramHost
    CoreModules *-- Overlays
    CoreModules *-- LightOutput
    CoreModules *-- StatusModel
    ProgramHost *-- DaylightProgram
    ProgramHost *-- OffProgram
    ProgramHost *-- RedAlertProgram
    ProgramHost *-- TuningProgram
    Program <|-- DaylightProgram
    Program <|-- OffProgram
    Program <|-- RedAlertProgram
    Program <|-- TuningProgram
    DaylightProgram *-- DaylightSchedule
    DaylightSchedule ..> solar
    StatusModel ..> solar : day info
    SkyModel ..> solar : the forecast by hour
```

The modules share nothing: each only knows the events it handles (its
`on(const T &)` overloads) and what it posts. `CoreModules` holds them, and
`wireCore()` (`core.cppm`) attaches them to the bus, the same in the
firmware and the host tests.

### Platform and the app (`sunsim::platform`, `main/main.cpp`)

```mermaid
classDiagram
    direction LR
    class App {
        -EventBus bus_
        -SettingsStore settings_
        -Clock clock_
        -InternetSync internet_
        -ZigbeeAdapter zigbee_
        -StatusSlot status_
        -CoreModules core_
        +run()
        -appTask()$
    }
    class ZigbeeAdapter {
        -atomic~uint16~ button_short_
        +start() / waitUntilReady()
        StackLock : lock() / unlock(), for std::scoped_lock
        +on(SetLight / Discover / Bind / Ping)
        +on(OpenNetwork / FactoryReset)
        +on(RoleAssigned / RoleCleared)
        -handleSignal / handleZclAction()
        -handleApsIndication() bool
    }
    class BulbEndpoint {
        <<value>>
        +turnOn / turnOff()
        +setLevel / setColorTemperature()
        +setColorXY / setHueSaturation()
    }
    class Clock {
        -ProgramConfig config_
        -unique_ptr~FastDayClock~ fast_day_
        +start()
        +on(Heartbeat)
        +on(ConfigChanged)
        +on(SetFastDay)
        -read() TimeReading
    }
    class InternetSync {
        -kJobs: name, fetch, Daily | Every
        -atomic sync window
        -per-job last_try, last_ok
        +start() waits for the boot sync
        +on(ConfigChanged)
        +on(SyncRequested)
        -run(everything) the due jobs, one session
        -syncClock() bool
        -fetchCloudCover() bool
        -fetchConditions() bool
        -fetchAlerts() bool
        -due(job, state, now) bool
    }
    class WifiSession {
        <<scoped>>
        connects to the best saved network
        disconnects and stops Wi-Fi when it ends
        +connected() bool
    }
    class FastDayClock {
        +now(ProgramConfig) tm
    }
    class SettingsStore {
        -ProgramConfig config_
        -Peaks peaks_
        +loadAll()
        +on(SetConfig / SetIntensity / SetMinLevel)
        +on(PersistRoles / FactoryReset)
    }
    class StatusSlot {
        -mutex
        -optional~StatusSnapshot~ snapshot_
        +on(StatusChanged)
        +get() optional~StatusSnapshot~
    }
    class Console {
        -Post post_
        -StatusSlot& status_
        +start() registers the commands
        +post(Event) / snapshot()
    }
    class StatusDisplay {
        <<functions>>
        startStatusDisplay(StatusSlot)
    }

    App *-- ZigbeeAdapter
    App *-- Clock
    App *-- InternetSync
    App *-- SettingsStore
    App *-- StatusSlot
    App *-- CoreModules
    App *-- EventBus
    ZigbeeAdapter ..> BulbEndpoint : SetLight as ZCL
    InternetSync ..> WifiSession : one per sync
    Clock *-- FastDayClock : in fast-day mode
    Console ..> StatusSlot : reads
    StatusDisplay ..> StatusSlot : reads
```

`ZigbeeAdapter` is the only code that includes esp-zigbee. It knows no
roles except the button's short address, which the APS hook needs at once
(below); `SetLight` already carries the bulb's address.

## 4. Threads

```mermaid
flowchart LR
    subgraph producers["posting threads"]
        direction TB
        ZT["Zigbee task<br/>signals, ZDO results, APS frames"]
        TM["esp_timer, 1 Hz"]
        CON["console REPL task<br/>pair, config, press, ..."]
    end
    TS["internet task<br/>boot, daily window, on request"] -.->|"sets (SNTP)"| SYS[("system clock")]

    ZT -->|radio events| Q
    TM -->|Heartbeat| Q
    CON -->|requests| Q
    Q[["EventBus<br/>pending events"]] --> W

    subgraph AT["app task"]
        direction TB
        W["bus.wait()"] --> L["ZigbeeAdapter::lock()"]
        L --> P["bus.process()<br/>every listener, in order"]
        P --> U["ZigbeeAdapter::unlock()"]
        U --> W
    end

    SYS -.->|"read by Clock<br/>on Heartbeat"| P
    P -.->|"StatusChanged:<br/>copy into"| SLOT[("StatusSlot<br/>(mutex)")]
    SLOT -.-> DT["display task (1 s)<br/>+ LVGL task"]
    SLOT -.-> CS["console: status, roles, config"]
```

- **All module code runs on the app task.** It holds the Zigbee lock for
  the whole batch, so handlers may issue Zigbee commands directly.
- `EventBus::process()` swaps the pending events out under its mutex and
  delivers them without it, so a Zigbee callback posting (already under
  the Zigbee lock) while the app task delivers can't deadlock.
- The esp_timer only posts `Heartbeat`; `Clock` reads the time on the app
  task and posts `Tick`, so the fast-day clock can follow `ConfigChanged`
  without a lock.
- **Two deliberate shortcuts** across threads, both small:
  1. The APS hook (Zigbee task) must decide *at once* whether to consume a
     frame. `ZigbeeAdapter` keeps the button's short address in an
     `std::atomic<uint16_t>` (set from `RoleAssigned`) and posts
     `ButtonPressed` for its frames.
  2. The console's `status` and the display read the latest
     `StatusSnapshot` from `StatusSlot`, which a `StatusChanged` listener
     fills under a mutex.
- Stacks: main 6 KB, esp_timer 4.5 KB, Zigbee 6 KB, console 6 KB, app 8 KB,
  display 4 KB, internet 8 KB (TLS). An `Event` is as big as its largest payload
  (~256 bytes) and is copied a few times on the poster's stack.

## 5. Event flow

Who posts what to whom. Solid arrows are events; each label is the event
type.

```mermaid
flowchart LR
    CK[Clock] -->|"Tick (1 Hz)"| MC & DR & SKY & PH & OV & SM
    CK -->|"ProgramTick (60 s)"| PH
    CK -->|"SyncRequested<br/>(fast-day switched)"| IS[InternetSync]
    CO -->|SyncRequested| IS
    CO -->|SetFastDay| CK
    ST -->|ConfigChanged| IS
    IS -->|"CloudCoverUpdated (daily),<br/>ConditionsUpdated, AlertsUpdated (15 min)"| SKY[SkyModel]
    CO -->|"ConditionsUpdated, AlertsUpdated<br/>(weather simulate)"| SKY
    SKY -->|"SkyChanged<br/>(when it changes)"| PH & OV & SM
    CO -->|ModeRequested| MC
    CO -->|"TuneLights, TuneSave"| PH
    CO -->|SetMinLevel| ST
    PH -->|"SetMinLevel (tune save)"| ST
    TMR(["esp_timer"]) -->|Heartbeat| CK

    ST[SettingsStore] -->|ConfigChanged| PH & OV & SM & CK
    ST -->|RolesLoaded| DR

    ZA[ZigbeeAdapter] -->|"DeviceAnnounced, EndpointFound,<br/>BindResult, PingResult, AckReceived"| DR
    ZA -->|ButtonPressed| MC & DR
    ZA -->|NetworkUp| DR & PH
    CO[Console] -->|"ButtonPressed (press)"| MC
    CO -->|"PairRequested, PairCanceled,<br/>UnpairRequested"| DR
    CO -->|OpenNetwork| ZA
    CO -->|FactoryReset| ST & ZA
    CO -->|SetConfig, SetIntensity| ST

    DR -->|"Discover, Bind, Ping, OpenNetwork"| ZA
    DR -->|PersistRoles| ST
    DR -->|RoleAssigned, RoleCleared| LO & ZA
    DR -->|ReachabilityChanged| LO
    DR -->|RefreshLights| PH
    DR -->|DevicesUpdated| SM

    MC -->|ModeChanged| PH & OV & SM

    PH -->|LightTarget| OV[Overlays]
    OV -->|OutputTarget| LO
    LO -->|SetLight| ZA

    SM -->|StatusChanged| SLOT[("StatusSlot")]
    SLOT -.-> CO & DI[StatusDisplay]
```

| Module | Listens to | Posts |
|---|---|---|
| **ZigbeeAdapter** | `SetLight`, `Discover`, `Bind`, `Ping`, `OpenNetwork`, `FactoryReset`, `RoleAssigned`, `RoleCleared` | `NetworkUp`, `DeviceAnnounced`, `EndpointFound`, `BindResult`, `PingResult`, `AckReceived`, `ButtonPressed` |
| **DeviceRegistry** | `RolesLoaded`, `NetworkUp`, `DeviceAnnounced`, `EndpointFound`, `BindResult`, `PingResult`, `AckReceived`, `ButtonPressed`, `PairRequested`, `PairCanceled`, `UnpairRequested`, `Tick` | `RoleAssigned`, `RoleCleared`, `Discover`, `Bind`, `Ping`, `OpenNetwork`, `PersistRoles`, `RefreshLights`, `ReachabilityChanged`, `DevicesUpdated` |
| **ModeController** | `ButtonPressed`, `ModeRequested`, `Tick` | `ModeChanged` |
| **SkyModel** | `CloudCoverUpdated`, `ConditionsUpdated`, `AlertsUpdated`, `Tick` | `SkyChanged` |
| **ProgramHost** (runs one `Program`) | `ModeChanged`, `ConfigChanged`, `SkyChanged`, `Tick`, `ProgramTick`, `RefreshLights`, `NetworkUp`, `TuneLights`, `TuneSave` | `LightTarget`, `SetMinLevel` (on `TuneSave`) |
| **Overlays** | `LightTarget`, `Tick`, `ModeChanged`, `ConfigChanged`, `SkyChanged` | `OutputTarget` |
| **LightOutput** | `OutputTarget`, `RoleAssigned`, `RoleCleared`, `ReachabilityChanged` | `SetLight` |
| **StatusModel** | `ConfigChanged`, `ModeChanged`, `DevicesUpdated`, `SkyChanged`, `Tick` | `StatusChanged` |
| **Clock** | `Heartbeat`, `ConfigChanged`, `SetFastDay` | `Heartbeat` (from its timer), `Tick`, `ProgramTick`; on a fast-day switch also `RefreshLights`, `SyncRequested` |
| **InternetSync** | `ConfigChanged` (the sync window), `SyncRequested` | `CloudCoverUpdated` (daily), `ConditionsUpdated` and `AlertsUpdated` (every 15 min); sets the system clock; runs on its own task |
| **SettingsStore** | `SetConfig`, `SetIntensity`, `SetMinLevel`, `PersistRoles`, `FactoryReset` | `RolesLoaded`, `ConfigChanged` |
| **Console** | — (REPL task; reads `StatusSlot`) | the requests above |
| **StatusDisplay** | — (display task; reads `StatusSlot`) | — |

### Programs

Every mode is a program (deriving from `core::Program`, for posting): it
decides what each bulb should show and posts `LightTarget`s, nothing else.
`ProgramHost` holds one of each in a `std::tuple` in `Mode` order (checked
at compile time, with each program's `kMode`) and runs the one the `Mode`
selects. No virtuals: the host calls what a program has, and an event
reaches a program only if it has `on()` for it.

| Member | Called on | Daylight | Off | Red alert | Tuning |
|---|---|---|---|---|---|
| `start(now)` (optional) | `ModeChanged` | refresh all | refresh all | bright, refresh all | every bulb at its minimum, amber |
| `refresh(role, now)` | `RefreshLights`, `NetworkUp`, `ConfigChanged` | sun curve now, 2 s | off | red at the current level | the tuned / minimum level |
| `on(Tick)` | while running | — | — | flip bright/dim, 0.9 s fade | — |
| `on(ProgramTick)` | while running | sun curve, ramped over the interval | — | — | — |
| `on(ConfigChanged)` | always | schedule, peaks, minimums | — | — | minimums, amber color |
| `on(SkyChanged)` | always | the cloud cover now | — | — | — |
| `on(TuneLights)` | always | — | — | — | the tuned levels and color |

On `TuneSave`, `ProgramHost` takes the tuning program's levels and posts
them as `SetMinLevel`, so programs still post only `LightTarget`s.

Only the running program gets ticks and refreshes, so no program checks
the mode. `LightOutput` doesn't know programs exist: it diffs each target
against what it last sent that bulb, so switching programs sends only what
differs (daylight -> red alert: hue/sat + level; each pulse after that:
level only), and it holds back bulbs marked unreachable.

### SkyModel: what the sky is now

The weather arrives as facts with a time: the daily forecast, and every 15
minutes the conditions and the alerts. Whether they still count is one
module's call: `SkyModel` re-judges on every `Tick` and each update - live
weather for `kWeatherFreshFor` (45 min), alerts until they end, the
forecast's cloud for the time (always, in fast-day) - and posts
`SkyChanged` whenever its conclusion changes. Everyone else just reacts:
the daylight program takes the cloud cover, `Overlays` the conditions and
alerts, `StatusModel` the lot. Nobody else keeps the weather or a clock to
judge it by.

### Overlays: the weather lights

`Overlays` sits between the programs and `LightOutput`. Each
`LightTarget` passes straight through as an `OutputTarget`, except on a
light the weather has taken over. It checks that on every `SkyChanged` and
`Tick` with `weatherPulse()`, a pure function of the light, whether its
program has it on, the mode, the settings and the `Sky`:

| On | When | Shows |
|---|---|---|
| `alert_lights` (default WallB, WallD) | an NWS warning, in daylight or off mode, lit or not | red, a 1 s fade |
| | an advisory / watch, in daylight mode, on a lit light | yellow 3 s / orange 2 s |
| `precip_lights` (default WallC) | rain, snow, ... falling, in daylight mode | a blue to violet per type; dimmer (35%) on an unlit light |

An alert wins where the two sets overlap. While a light pulses, its
program's targets are kept but not sent. When the sky lets it go (at once,
on `SkyChanged`; a dead connection does it once the weather goes stale),
the latest one goes out with a 2 s transition.

To add a program: a `Mode` (the enum, `kModes`, `modeName`), a `Program`
subclass, a member and its case in `ProgramHost::program()`, and whatever
selects it (`ModeController`, the console). Nothing else changes.

### Flows

Behavior that happens in steps over time is a coroutine (`ev::Flow`)
rather than handlers, flags and Tick counters. `DeviceRegistry`'s endpoint
discovery, per role:

```cpp
ev::Flow DeviceRegistry::findEndpoint(ev::Role role)
{
    if (!network_up_) {
        co_await network_.next(); // roles restored at boot: ask once there's a network
    }
    for (;;) {
        const ev::Instant retry_at = timer_.now() + kDiscoveryRetry;
        post_(ev::Discover{role, record(role).short_addr});
        while (const auto found = co_await endpoints_.next(about(role), retry_at)) {
            if (found->endpoint != 0) {
                co_return endpointFound(role, found->endpoint);
            }
        }
    }
}
```

| Piece | What it is |
|---|---|
| `Flow` | a coroutine owned by its module; starts at once; reassigning or destroying it cancels it wherever it waits |
| `Timer` | `co_await timer.until(t)` / `after(15s)`; the module's `on(Tick)` drives it, and timestamped events move it forward |
| `Inbox<T>` | `co_await inbox.next(filter, deadline)`: the next `T` the flow wants, or `nullopt` at the deadline; the module's handler calls `deliver()` |

The flows: `ModeController::run` (single vs double press by timing),
`DeviceRegistry::findEndpoint` (per role, every 15 s until found) and
`keepAlive` (per bulb: first ping 15 s after the network is up, then every
60 s; a failed ping plus 60 s of silence marks it unreachable, and it's
pinged every 15 s until it answers). The host tests run them unchanged.

## 6. Key sequences

### Boot

```mermaid
sequenceDiagram
    participant M as app_main / App
    participant ST as SettingsStore
    participant CK as Clock
    participant ZA as ZigbeeAdapter
    participant Q as EventBus (app task)
    participant DR as DeviceRegistry
    participant SKY as SkyModel
    participant PH as ProgramHost
    participant LO as LightOutput

    M->>M: nvs_flash_init, TZ, construct modules, wire
    M->>ST: loadAll()
    ST-->>Q: RolesLoaded, ConfigChanged (queued)
    M->>M: InternetSync.start() - its task syncs the clock and fetches the forecast, App waits
    Note over M,Q: CloudCoverUpdated (queued)
    M->>CK: start() - 1 Hz Heartbeat
    M->>ZA: start() - Zigbee task forms / rejoins the network
    M->>Q: app task: waits for the stack, then processes
    Q->>DR: RolesLoaded
    DR-->>Q: RoleAssigned per paired role
    Note over DR: discovery / keep-alive flows wait for NetworkUp
    Q->>LO: RoleAssigned: bulb addresses
    Q->>ZA: RoleAssigned: the button's address
    Q->>PH: ConfigChanged
    Q->>SKY: CloudCoverUpdated
    SKY-->>Q: SkyChanged (to the programs, overlays, status)
    loop every second
        Q->>CK: Heartbeat
        CK-->>Q: Tick (+ ProgramTick every 60 s)
    end
    ZA-->>Q: NetworkUp
    Q->>DR: flows resume: Discover (endpoint unknown), Ping after 15 s
    Q->>PH: refresh all
    PH-->>Q: LightTarget per bulb
    Q->>LO: LightTarget
    LO-->>Q: SetLight (bulbs with an address)
    Q->>ZA: SetLight: ZCL commands
```

### Daylight update (every minute)

```mermaid
sequenceDiagram
    participant CK as Clock
    participant PH as ProgramHost / DaylightProgram
    participant LO as LightOutput
    participant ZA as ZigbeeAdapter
    participant B as Bulb

    CK->>PH: ProgramTick(time, 60 s)
    loop each bulb
        PH->>PH: wallState / workState, then peak %
        PH->>LO: LightTarget(role, state, 60 s)
        alt address known, reachable, something changed
            LO->>ZA: SetLight(only the changed fields)
            ZA->>B: ZCL Move to Level / Color Temperature / Color XY
        else unchanged, unknown or unreachable
            Note over LO: nothing sent
        end
    end
```

How the wall's targets are computed is in DESIGN.md §4.

### Button double press → red alert

```mermaid
sequenceDiagram
    participant B as Button
    participant ZA as ZigbeeAdapter
    participant Q as EventBus
    participant MC as ModeController
    participant PH as ProgramHost
    participant LO as LightOutput

    B->>ZA: ZCL On (0x0006 / 0x01) to endpoint 1
    Note over ZA: APS hook, Zigbee task:<br/>source == the button's address
    ZA->>Q: ButtonPressed(Double)
    ZA-->>B: frame consumed
    Q->>MC: ButtonPressed(Double)
    MC->>Q: ModeChanged(RedAlert)
    Q->>PH: RedAlertProgram.start()
    PH->>Q: LightTarget(red, bright) per bulb
    Q->>LO: hue/sat + level changed
    LO->>Q: SetLight per bulb
    loop every Tick while red alert
        Q->>PH: Tick: flip bright / dim
        PH->>Q: LightTarget(red, 0.9 s fade) per bulb
        Q->>LO: only the level changed
        LO->>Q: SetLight(level) per bulb
    end
```

Pairing a device (console `pair` or the role manifest) is in DESIGN.md §2.

## 7. Event catalogue (`events.cppm`)

| Event | Payload | Posted by |
|---|---|---|
| `Heartbeat` | — | Clock's 1 Hz esp_timer |
| `Tick` | `at` (boot time), `TimeReading` (local time, UTC offset, valid, simulated) | Clock, 1 Hz |
| `ProgramTick` | `TimeReading`, transition | Clock, every 60 s (1 s in fast-day) |
| `SetFastDay` | on, real seconds per simulated day | Console (`fastday`) |
| `SyncRequested` | — | Console (`sync`), Clock (fast-day switched) |
| `ConfigChanged` | `ProgramConfig`, per-light `Peaks` and `MinLevels` | SettingsStore (at boot, then on change) |
| `CloudCoverUpdated` | `CloudCover` (48 local hours) | InternetSync, daily |
| `ConditionsUpdated` | `Conditions` (cloud %, `Precip`, `Intensity`), `at` | InternetSync, every 15 min; Console (`weather simulate`) |
| `AlertsUpdated` | `Alerts` (up to 4: tier, event name, `until`), `at` | InternetSync, every 15 min; Console (`weather simulate`) |
| `SkyChanged` | `Sky`: cloud % (live or forecast), live `Conditions`, `Alerts` in force | SkyModel, when it changes |
| `SetConfig` | `ProgramConfig` | Console (`config`) |
| `SetIntensity` | `Selection` (all, a group or a role), percent | Console (`intensity`) |
| `SetMinLevel` | `Selection`, level 1-254 | Console (`minlevel`), ProgramHost (`tune save`) |
| `ModeRequested` | `Mode` | Console (`tune`, `tune off`) |
| `TuneLights` / `TuneSave` | `Selection`, level, optional kelvin / — | Console (`tune`) |
| `RolesLoaded` / `PersistRoles` | every `DeviceRecord` | SettingsStore / DeviceRegistry |
| `NetworkUp` | rejoined, `at` | ZigbeeAdapter |
| `DeviceAnnounced` | ext addr, short addr | ZigbeeAdapter |
| `EndpointFound` | role, endpoint (0 = none) | ZigbeeAdapter |
| `BindResult` / `PingResult` | role, ok (+ error, status) | ZigbeeAdapter |
| `AckReceived` | short addr, RSSI, ZCL status | ZigbeeAdapter |
| `ButtonPressed` | `Single` / `Double` / `Hold` / `Ambiguous`, source, `at` | ZigbeeAdapter, Console (`press`) |
| `PairRequested` / `PairCanceled` | role / — | Console |
| `UnpairRequested` | `Selection` | Console |
| `OpenNetwork` | duration (s, default 180) | Console, DeviceRegistry |
| `FactoryReset` | — | Console |
| `RoleAssigned` / `RoleCleared` | role (+ `DeviceRecord`) | DeviceRegistry |
| `Discover` / `Bind` / `Ping` | role, short addr (+ ext addr, endpoint for `Bind`) | DeviceRegistry |
| `ReachabilityChanged` | role, reachable | DeviceRegistry |
| `DevicesUpdated` | per-role `DeviceStatus`, records, armed role | DeviceRegistry, 1 Hz |
| `ModeChanged` | `Mode` (`Daylight` / `Off` / `RedAlert`) | ModeController |
| `RefreshLights` | role, or every light | DeviceRegistry |
| `LightTarget` | role, `LightState`, level and color transitions | the running program |
| `OutputTarget` | the same: what the bulb does show | Overlays |
| `SetLight` | short addr, endpoint, optional power / level / `Color`, transitions | LightOutput |
| `StatusChanged` | `shared_ptr<const StatusSnapshot>` | StatusModel, 1 Hz |

## 8. Wiring

```cpp
// A module handles an event by having an overload for it:
class LightOutput {
public:
    void on(const ev::RoleAssigned &e);
    void on(const ev::OutputTarget &e);
    // ...
};

// sunsim_core (core.cppm), shared by firmware and tests:
void wireCore(ev::EventBus &bus, CoreModules &m)
{
    bus.attach(m.mode, m.registry, m.programs, m.overlays, m.output, m.status);
}

// main (main.cpp): the platform modules.
bus_.attach(settings_, clock_, internet_, zigbee_, status_);
```

`attach()` walks the `Payload` variant at compile time and subscribes the
module to each `T` it has exactly `void on(const T &)` for (the
`HandlerOf` concept). A handler that would take an event only through a
conversion - by value, or as a base class - doesn't compile
(`TakesByConversion`), so a mistyped handler can't go silently unwired.
Event names for tracing come from the types too (`kEventNames`).

Listeners are added before anything is processed, and an event's run in the
order they were attached. What a listener posts is delivered on a later
pass, so that order only matters for effects outside the bus: on
`FactoryReset`, `SettingsStore` erases the roles before `ZigbeeAdapter`'s
reset reboots.

## 9. Testing

`test/host/` builds `sunsim.ev`, `sunsim.core` and `sunsim.weather` with plain CMake on the
PC (no ESP-IDF; exceptions and RTTI off like the firmware), attaches them with
the same `wireCore()`, and asserts on everything posted in response to the
events each test posts:

```
./sunsim test
```

One file per area (`devices_tests.cpp`, `modes_tests.cpp`,
`daylight_tests.cpp`, `weather_tests.cpp`, `events_tests.cpp`), sharing the
module `sunsim.test` (`harness.cppm`: the `Harness`, a fixed Tuesday to run
on, the runner) and `check.h` (`CHECK`, a macro, so a header).

Covered: console pairing end to end; a paired device is never re-claimed by
`pair`; unknown devices ignored; double press -> red pulses -> single press
re-syncs; hold -> all off; ambiguous On/Toggle timing; clock-not-set holds
lights off; per-light intensity; liveness (acks -> online, silence ->
silent, pings, an unanswered ping counts as failed); discovery retry and
unpairing stopping it; button pairing binds; `unpair all` and unpairing a
group; the role layout; the wall dimming in order away from the sun; the
name-keyed role manifest; the amber sweep across the wall at sunset; the
cloud forecast lookup and its overlay (dimmer, cooler, diffuse); live
cloud cover winning while fresh; the streaming JSON scanner and the
Open-Meteo / NWS readers (fed a few bytes at a time); the weather lights
(snow pulses and lets go, dimmer when unlit, none in off mode; a warning
wakes lights in off mode, a watch doesn't; alerts outrank precipitation;
stale weather releases them); minimum
levels keeping the wall lit to sunset; tuning mode and `tune save`; the
bus delivering 40k events from two threads in each poster's order.

## 10. The EventBus

- Standard library only (`bus.cppm`): a `std::mutex`-guarded vector of
  pending events, a `std::condition_variable` for `wait()`, and a
  `std::function` list per payload type.
- `process()` swaps the pending events out and delivers them without the
  mutex, so posters never block on listeners. The two vectors keep their
  capacity, so nothing allocates once warmed up.
- `StatusChanged` carries a `shared_ptr` because it's by far the largest
  payload and every `Event` is as big as its largest alternative.
- ThreadSanitizer can't run: GCC 15's `std` module doesn't build with
  `-fsanitize=thread`.

## 11. History

- The code started as a few large classes (`LightRig` did pairing,
  discovery, liveness, scheduling, intensity and Zigbee output) reached
  from six threads under hand-placed Zigbee locks. The event bus replaced
  that in one branch; NVS keys stayed the same, so pairings carried over.
- Since then: every mode became a `Program` (red alert used to be split
  across the mode controller and the output); missing bulbs are tolerated
  (an unreachable bulb used to tie up the stack's buffers with retries);
  multi-step behavior moved into coroutine flows; the components became
  C++ modules; the bus wrapped eventpp until clangd couldn't build it next
  to `import std`, and is now its own (under 100 lines); roles became groups
  (`kLayout`) for the five-light wall. Then: modules attach themselves to
  the bus by their `on()` overloads; `SkyModel` judges the weather for
  everyone; the settings became one table; time became `<chrono>`, colors
  a `std::variant`.
- Considered and rejected: ESP-IDF's `esp_event` (C-only, memcpy'd POD
  payloads, no host build).

## 12. Compiler workarounds (GCC 15, C++ modules)

Each is commented where it is; undo them as GCC's modules mature.

| Where | What | Why |
|---|---|---|
| `Event`'s copy/move/destructor (`events.cppm`), `Timer::~Timer` (`flow.cppm`), `Program::~Program` (`program.cppm`) | defined out of line, not implicit or inline | importers otherwise link to `std::variant` / `vector` internals nobody emitted |
| `DaySpan`, `Mireds`, `Xy`, `HueSat` (`types.cppm`) | `operator==` spelled out, not defaulted | GCC crashes compiling a file that imports them and compares them |
| `parseRoleSet` (`types.cppm`) | a `find` loop, not `views::split` | its `subrange` fails to compile across modules |
| `InternetSync` (`internet_sync.cppm` / `.cpp`) | definitions in a module implementation unit; the thread behind a `Task` | `std::jthread` and the C++20 semaphores in a `.cppm` crash GCC writing the module interface |
| `kEventNames` (`events.cppm`) | names parsed from `__PRETTY_FUNCTION__` | no reflection until C++26 |

The FreeRTOS tasks (Zigbee, app, display) stay FreeRTOS tasks: as
`std::jthread`s each would need its own implementation unit, for the same
crash.

## 13. Open questions

- One queue is ample at a few events a second; should time-critical
  `SetLight` traffic ever bypass the rest?
- NVS writes happen on the app task while it holds the Zigbee lock; fine at
  today's rates.
- `DevicesUpdated` (~250 B) now sets every `Event`'s size; share it like
  `StatusChanged` if events grow further.
- A presence sensor is planned to replace the fixed active window; the gate
  in `DaylightSchedule` is the place it plugs in.
