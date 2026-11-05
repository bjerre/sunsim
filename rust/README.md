# sunsim in Rust: the pure layers

A port of everything that doesn't touch ESP-IDF, as a cargo workspace:

| Crate | Mirrors | Holds |
|---|---|---|
| `ev` (sunsim-ev) | `components/sunsim_events` | value types, events, the bus, flows, settings, the radio contract, the room |
| `sunsim-core` | `components/sunsim_core` | the registry, modes, programs, overlays, output, send queue, sky and status models, the math |
| `weather` (sunsim-weather) | `components/sunsim_weather` | the streaming JSON scanner and the Open-Meteo / NWS readers |
| `platform` (sunsim-platform) | `components/sunsim_platform`, the logic | the platform modules without ESP-IDF, their hardware behind traits: the Clock, the SettingsStore (on a `Storage`: NVS's typed keys), the InternetSync (on an `Internet`: Wi-Fi sessions, SNTP, HTTPS), the saved Wi-Fi networks and the console's commands |
| `sim` (sunsim-sim) | `sim/` | virtual time, the simulated clock and Zigbee network, `World` |
| `tests` (sunsim-tests) | `test/host` | the harness (`src/lib.rs`) and the host tests, one file per area |

    cargo test                 # everything
    cargo test --test sim      # one area
    cargo test ambiguous       # tests whose names match

## How the C++ idioms map

- **`EventBus::attach` finding `on(const T &)` overloads** → `Module::on(&mut self, &Event)` with
  a `match`; anything else is ignored. A module is attached as `Rc<RefCell<M>>`.
- **`ev::Radio` (a concept)** → the `Radio` trait: a method per request, so a radio that misses
  one doesn't compile; `radio::requests()` routes the events to them.
- **Coroutine flows** (`Flow`, `Timer`, `Inbox`) → async blocks, resumed inline by the Timer or
  Inbox they wait on (no executor). Dropping a `Flow` cancels it. A flow can't borrow its module,
  so each module with flows splits into the module (handlers, flow slots) and an `Rc<Shared>`
  (post, timer, inboxes, and a `RefCell<State>` never held across an `.await`).
- **`consteval` room checks** (`topology_of`, `addresses_of`, the settings keys) → `const fn`s
  evaluated into `const` items: a `panic!` there is a compile error with its message.
- **Pointers to `ProgramConfig` members** (`Tunable`, the settings table) → `Field<T>`, a getter
  and setter pair made by `field!(name)`; `setting!(name)` is a `Tunable::Setting`.
- **`std::chrono`** → `ev::time`: a signed microsecond `Duration` and boot `Instant`, local
  `LocalSeconds`/`LocalDays`, UTC `SysSeconds`, Zigbee `Transition` (tenths).
- **Logging** → the `log` crate, with the C++ tags as targets (esp-idf-svc's logger takes it
  on the board).

## The firmware (firmware/)

The board's side, in progress: its own crate, outside the workspace, for the C6's
`riscv32imac-esp-espidf` target (std on ESP-IDF through esp-idf-svc, the eim-installed
IDF v6.1, a pinned nightly). `firmware/build.sh --release` builds it with IDF's
environment loaded; `firmware/flash.sh` flashes the app alone (ota_0), keeping the bootloader, partition table and NVS.

So far: the core, the Clock (`platform::Clock` on `src/clock.rs`'s timers and time zone)
the SettingsStore (`platform::SettingsStore` on `src/storage.rs`'s NVS, the C++
firmware's keys and widths), the InternetSync (`platform::InternetSync` on
`src/internet.rs`: esp-idf-svc's Wi-Fi, SNTP and HTTP client) and the Zigbee adapter on
the app task, as `main/main.cpp` runs them. Menuconfig's
settings come from the build's environment instead (`src/config.rs`:
`SUNSIM_TZ=... ./build.sh --release`). `src/zigbee/` is the C++ `ZigbeeAdapter` ported: `ffi.rs` (the C
names, the raw reads), `bulb.rs` (the ZCL light commands), `mod.rs` (the stack task, its
callbacks, `ev::Radio`). esp-zigbee-lib comes through `components/zigbee_shim`, which
depends on it and gives bindgen its function-like macros as functions. Not yet run on a
board. Still to port: the display.
