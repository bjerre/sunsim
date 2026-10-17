# sunsim

An ESP32-C6 Zigbee coordinator that drives 6 Zigbee bulbs through a daylight
simulation and responds to one physical button controller:

- **5 "wall" bulbs** fade through a warm-to-cool, dim-to-bright curve
  between the real local sunrise and sunset, simulating daylight.
- **1 "work" bulb** stays bright and shifts cool to warm across the work
  hours (default 08:30-17:30: ~6000K in the morning to ~3000K by
  17:30, then holds warm).
- The wall bulbs sit east-to-west on the south wall (WallA east, WallE
  west): the one nearest the real sun's direction is brightest (`config sunshift`),
  and near sunrise/sunset they glow amber, deepest at the end nearest the
  sun (`config amber`, `config ambersweep`).
- The **real sky** shapes the wall: the cloud cover now (Open-Meteo,
  every 15 minutes; the hourly forecast if that's missing) makes cloudy
  hours dimmer, cooler and diffuse (no sun direction, no golden glow) -
  `config clouds`.
- **Weather lights**: one wall light (WallC) pulses while rain or snow
  falls, blue for rain, pale ice blue for snow, violet for freezing rain,
  purple for thunder, glowing dimly even outside active hours. Two more
  (WallB, WallD) pulse yellow, orange or red for a National Weather Service
  advisory, watch or warning; a warning turns them on whatever the hour,
  in daylight or off mode. `config weather`, `config precip`, `config alerts`,
  and the `weather` command.
- All bulbs only run inside an **active window** (default Mon-Fri,
  08:00-19:00, 10 min fade at each edge) and are off outside it.
- **1 button controller** (any simple single-button Zigbee switch):
  - single press -> resume the daylight program
  - long press (Off) -> all lights off until pressed again
  - double press -> a "red alert" strobe effect on all the bulbs

sunsim forms and is the coordinator of its own small Zigbee network - it
does not join an existing hub's network. Built on
[esp-zigbee-sdk](https://github.com/espressif/esp-zigbee-sdk), targeting
ESP32-C6, written in C++20.

See [DESIGN.md](DESIGN.md) for how devices, roles and the button work and
the math behind the daylight curve, and [ARCHITECTURE.md](ARCHITECTURE.md)
for the modules, class diagrams, threads, events and boot sequence.

## Building

Needs ESP-IDF 6.1 (GCC 15; the code uses C++20 and later). The `./sunsim`
script loads ESP-IDF's environment itself, so it works from any terminal:

```
./sunsim go              # build, flash, then the serial monitor/console (Ctrl+] quits)
./sunsim build | flash | monitor
./sunsim cmd status      # one console command on the running board, reply printed
./sunsim cmd pair WallB
./sunsim test            # host tests
./sunsim idf menuconfig  # any idf.py command
```

It finds the board as the only `/dev/ttyACM*` (or set `SUNSIM_PORT`), and
uses `~/.espressif/v6.1/esp-idf` (or set `SUNSIM_IDF`). `cmd` doesn't reset
the board, and can't run while `monitor` holds the port.

By hand, `idf.py` only works in a shell where ESP-IDF's environment has
been loaded (once per terminal; `export.sh`, not the `activate_idf` script,
whose Python the build directory refuses):

```
. ~/.espressif/v6.1/esp-idf/export.sh
```

Then, from this directory:

```
idf.py set-target esp32c6   # first time only - it regenerates sdkconfig
idf.py menuconfig           # "Sunsim Configuration": TZ, latitude/longitude, channel
idf.py -p /dev/ttyACM0 flash monitor   # builds too; Ctrl+] quits the monitor
```

The VS Code ESP-IDF extension's Build/Flash/Monitor buttons do the same
without the `export.sh` step.

Code navigation and diagnostics come from clangd (VS Code's clangd
extension, using ESP-IDF's own clangd; see `.clangd` and
`.vscode/settings.json` - Microsoft's C/C++ IntelliSense is turned off). It
reads `build/compile_commands.json`, so build once first. The host tests
are read with the firmware's flags too: clangd
gives each module one compile command, so everything importing the
components' modules has to match them. clangd builds its own copies of the
C++ modules (`--experimental-modules-support`), so the first file opened
takes a couple of minutes; after a change to `.clangd` or the build, run
"clangd: Restart language server".

## Missing devices

A bulb that stops answering (switched off at the wall, out of range) is
detected within about a minute (`REGISTRY: ... not answering - pausing its
light commands`) and gets no commands until it answers again; the other
lights carry on normally. It's checked every 15 s and its light is
re-applied as soon as it's back. `status` shows it as `silent`.

## Active hours

```
sunsim> config                       # every setting: its value and what it does
sunsim> config x                     # what each one takes
sunsim> config hours 08:00-19:00
sunsim> config workhours 08:30-17:30   # work light's cool -> warm span
sunsim> config sunshift 25          # wall dimming away from the sun, % (0 = off)
sunsim> config amber on             # amber glow when the sun is < 6 deg up
sunsim> config ambersweep 3         # the glow follows the sun: far end lags 3 deg (0 = all alike)
sunsim> config fade 10               # minutes to fade in/out at each edge (0 = hard on/off)
sunsim> config days mon-fri          # or: all, or a list like mon,tue,thu
sunsim> config syncwindow 04:00-05:00   # daily clock sync and cloud forecast
sunsim> config clouds on            # cloud cover shapes the wall (off: always clear sky)
sunsim> config clouddim 40          # full overcast: up to 40% dimmer
sunsim> config cloudcool 50         # ... and 50% of the way to the cool end
sunsim> config weather on           # the weather lights (off: every light follows its program)
sunsim> config precip WallC         # which lights show rain and snow (or none)
sunsim> config alerts WallB,WallD   # which show NWS advisories/watches/warnings (or none)
```

Settings are stored on the device and survive reflashing; changes apply
on the next update.

## Light intensity

Each light has a peak intensity, 1-100% of the bulb's full brightness,
for balancing the lights in your scene. It scales that light's whole
program (not red alert), is stored on the device, and is kept across
reflashing and re-pairing:

```
sunsim> intensity              # list (defaults: walls 100, work 90)
sunsim> intensity WallA 80     # applies within 2s
sunsim> intensity wall 60      # every wall light
sunsim> intensity all 75
```

## Minimum levels and tuning

Most bulbs go dark below some low level (and it differs by bulb), which
cuts them off before the real sunset. Each light has a **minimum level**
(1-254, raw Zigbee level, default 1): while it's on, its daylight curve
runs from that minimum up to its peak, so it stays visibly lit until
sunset (or the end of the active window), then turns off.

Find each bulb's minimum in tuning mode, near sunset or in a dark room:

```
sunsim> tune                  # every bulb steady at its minimum, in sunset amber (1800K)
sunsim> tune wall 6           # try 6 on the whole wall
sunsim> tune WallC 9          # WallC needs more
sunsim> tune WallA 8 2200     # a level and a color (1700-6500K; below 2700K is sent as xy)
sunsim> tune save             # the levels set since `tune` become the minimums
sunsim> tune off              # back to daylight (or press the button once)
sunsim> minlevel              # list; `minlevel WallB 7` sets one directly
```

## Status display

The board's LCD (Waveshare ESP32-C6-GEEK, 1.14" ST7789) shows:

- the time, large, and the weekday ("sim" in fast-day mode)
- the mode: `daylight`, `idle` (outside active hours), `off` or `RED ALERT`
- today's day length, and the countdown to the next sunrise or sunset
- one dot per device, grouped `D ●●●●●  W ●  B ●`: D = the daylight wall
  (WallA-WallE, east to west), W = the work light, B = the button;
  green = responding, amber = paired but silent, gray = not paired

The `status` console command prints the same information. Pins and
orientation are under menuconfig, "Sunsim Configuration" -> "Status
display"; "Rotate the picture 180 degrees" flips it if it's upside down.

## Wi-Fi (internet sync)

sunsim connects to Wi-Fi briefly, using the strongest saved network in
range, then turns Wi-Fi off again. All for the menuconfig
latitude/longitude, free and without keys:

- **once a day** in the sync window (default 04:00-05:00, before the
  lights' day), retrying every 10 minutes within it if it fails: the clock
  over SNTP and the hourly cloud-cover forecast for today and tomorrow
  (Open-Meteo);
- **every 15 minutes**, around the clock: the weather now (Open-Meteo's
  15-minute model data: cloud cover and what's falling) and the active
  alerts (api.weather.gov, US only);
- everything at boot, before the Zigbee radio starts, and whenever asked:
  `sync`, or switching fast-day mode on or off;
- every 10 minutes while the clock has never been set (the lights stay off
  until it is).

Live weather older than 45 minutes is ignored: the wall goes back to the
forecast's clouds and the weather lights to their program.

```
sunsim> weather                       # cloud, what's falling, alerts, the weather lights
sunsim> weather simulate snow heavy   # pretend, to see the lights (until the next fetch)
sunsim> weather simulate warning
sunsim> weather simulate clear
```

```
sunsim> wifi add "My Network" mypassword
sunsim> sync                          # now (watch the INTERNET log lines)
sunsim> config syncwindow 04:00-05:00
```

Networks are stored on the device and survive reflashing. Wi-Fi shares the
C6's one radio with Zigbee (ESP-IDF's Wi-Fi/802.15.4 coexistence), so a
sync while Zigbee runs takes 10-20 s rather than the few seconds at boot;
the clock sync waits up to 2 minutes for an answer.

`wifi` alone lists the saved networks, `wifi remove <ssid>` forgets one and
`wifi clear` forgets all. The Wi-Fi SSID/password in menuconfig still work
as a fallback when none is saved from the console.

Latitude/longitude (decimal degrees, North/East positive) drive the wall
bulbs' sunrise/sunset calculation - see
[DESIGN.md](DESIGN.md#5-how-location-is-provided). Left at the default
`0.0`/`0.0`, you get the equator/Greenwich meridian (sunrise/sunset close to
06:00/18:00 year-round).

## Pairing

Roles (`WallA`-`WallE` east to west along the wall, `Work`, `Button`; the
counts per group are `ev::kLayout` in `types.cppm`, the one place to change
for more or fewer wall lights) are assigned from the serial
console (`idf.py monitor`, over the same USB cable used for flashing), one
device at a time:

```
sunsim> pair WallA        # arms WallA and opens the network for 180s
   ...factory-reset / power on bulb #1; the log shows "Paired ext=0x... -> WallA"
sunsim> pair WallC
   ...factory-reset bulb #2
sunsim> roles             # check: WallA and WallC assigned, endpoint found
```

The armed role goes to the next device that joins *and doesn't already hold
a role*. That way a paired bulb that gets power-cycled during pairing can't
take the wrong role. To reassign a device, `unpair <role>` it first. Other
commands:

- `unpair <role|group|all>` - forget role assignments (e.g. `unpair WallB`,
  `unpair wall`)
- `open [seconds]` - reopen the network for joining (it opens for 180s at boot)
- `pair cancel` - disarm a pending `pair`
- `factory_reset` - forget all roles **and** the Zigbee network, then restart
  with a fresh network. The bulbs must then be factory-reset too, since
  they still remember the old network.

Roles and the Zigbee network both live in the `nvs` partition, so they survive
reflashing. `idf.py erase-flash` also wipes both. Upgrading from the
three-light wall renames the stored roles once, keeping each bulb's place:
`WallB` (the middle) becomes `WallC` and `WallC` (the west end) becomes
`WallE`; pair the new bulbs as `WallB` and `WallD`.

Alternatively, devices can be pre-assigned by IEEE address in
[`main/role_manifest.json`](main/role_manifest.json) (used only when no `pair`
is armed). Use the exact `ext=` value from the log, not a hand-copied label.
Entries are by role name; one the layout doesn't have fails the build.

You don't need all 7 devices present for sunsim to do something useful:
whichever bulbs have joined get the program applied; missing ones are
silently skipped.

## Button

Tested with a SONOFF SNZB-01-style button, which says which press it was:

| Press  | Button sends | sunsim does            |
|--------|--------------|------------------------|
| single | `Toggle`     | resume daylight program |
| double | `On`         | red alert (TNG pulse)  |
| hold   | `Off`        | all lights off         |

These buttons send to sunsim's endpoint 1 regardless of the binding, where
the stack has no On/Off server and would drop them, so frames from the
paired button are picked out at the APS layer and dispatched by command
(`platform::ZigbeeAdapter::handleApsIndication`). Buttons that instead send to the bound
receiver endpoint 2 still go through the older path: the On/Off attribute
change, with single vs. double press resolved by timing
(`ZigbeeAdapter::handleZclAction` posts `Press::Ambiguous`, resolved in
`core::ModeController`).

For a different button, set the `SUNSIM` log tag to DEBUG
(`esp_log_level_set("SUNSIM", ESP_LOG_DEBUG)` or menuconfig): every
incoming application frame is then logged as
`RX from 0x2fed ep1 -> ep1 cluster 0x0006 cluster cmd 0x02 ...`,
which shows what it really sends.

### Testing without the button

The console's `press` command feeds the same button logic, so you can try
every mode before a button is paired:

```
sunsim> press double   # red alert
sunsim> press single   # resume the daylight program
sunsim> press hold     # all lights off
```

## Debug fast-day mode

To bench-test the daylight curve without waiting for a real sunrise/sunset:

```
sunsim> fastday on        # one active window every 120 s
sunsim> fastday on 30     # ... every 30 s
sunsim> fastday off       # back to real time
sunsim> fastday           # which is it?
```

In fast-day mode the program's clock loops the active window (`config
hours`, e.g. 08:00-19:00) every so many seconds of real time, stepping
through the active days only (the off hours and weekends are skipped), and
the lights update every second instead of every minute so the gradient
visibly animates. The display shows "sim" by the weekday; `status` says
"(simulated)". Switching shows the new time within 2 s and runs an
internet sync. It isn't saved: the board boots in real time unless
`SUNSIM_DEBUG_FAST_DAY` is set in menuconfig (`SUNSIM_DEBUG_DAY_SECONDS` is
the default length). It works with any subset of the devices paired.

## Smooth transitions

Each periodic update sends the bulb's level/color-temperature commands with
a transition time matching the update interval (60s normally, 1s in debug
fast-day mode), so a bulb fades continuously from its current value to the
next target rather than snapping and holding. Explicit state changes that
should feel immediate instead - resuming after an override, a bulb
finishing discovery, a reboot, a setting change - use a short fixed 2s
transition (`RefreshLights`) rather than waiting for the next slow tick to
catch up.

## Layout

Event-driven: every module talks only through events on one bus, processed
by a single app task. See [ARCHITECTURE.md](ARCHITECTURE.md).

Each component is one C++ module (`import sunsim.ev;` and so on, plus
`import std.compat;`): each `.cppm` is a partition holding one class (or
one piece of math), declaration and definitions together, and a
component's main `.cppm` re-exports the partitions (one exception:
`InternetSync`'s definitions are in `internet_sync.cpp`, see ARCHITECTURE.md
§12). The only headers left are the generated `role_manifest.h` and the
tests' `check.h`; logging is `ev::log::info(tag, "{}", ...)`, std::format
strings checked at compile time.

- `components/sunsim_events/` (`sunsim::ev`, module `sunsim.ev`) - the
  shared vocabulary: value types (`types.cppm`), every event
  (`events.cppm`), the settings table (`settings.cppm`: what `config`
  shows and takes, and the NVS key of each), the `EventBus` (`bus.cppm`), coroutine flows
  (`flow.cppm`: `Flow`, `Timer`, `Inbox`), and `BootClock::now()`.
- `components/sunsim_core/` (`sunsim::core`, module `sunsim.core`) - all
  application logic, pure C++ (builds on the host):
  - `device_registry.cppm` - pairing, endpoint discovery/binding, liveness pings.
  - `mode_controller.cppm` - single/double/hold -> daylight / red alert / off.
  - `programs/` - the lighting programs, one per mode:
    - `program.cppm` - the interface every program implements (what each bulb
      should show); `program_host.cppm` runs the selected one.
    - `daylight_program.cppm` - sun curve, work light, active window, sun
      shift, amber, cloud cover, per-light minimum-to-peak range, clock guard.
    - `red_alert_program.cppm`, `off_program.cppm`, `tuning_program.cppm` -
      the other modes.
  - `overlays.cppm` - the weather lights: between the programs and the
    output, pulses chosen lights for rain, snow and NWS alerts.
  - `light_output.cppm` - turns any program's targets into ZCL commands, diffed
    against what was last sent.
  - `status_model.cppm` - the snapshot shown by `status` and the display.
  - `sky_model.cppm` - what the sky is now (forecast, live conditions and
    alerts while they count), posted as `SkyChanged`.
  - `math/` - `daylight.cppm`, `solar.cppm`, `sky.cppm` (the forecast's
    cloud cover by hour), `day_info.cppm`: the pure math.
  - `core.cppm` - `CoreModules` and `wireCore()`, which attaches them to the bus.
- `components/sunsim_weather/` (`sunsim::weather`, module `sunsim.weather`)
  - reading the weather services' replies as they stream in, pure C++
  (host-tested): `json.cppm` (`JsonScanner`), `readers.cppm` (Open-Meteo's
  forecast and conditions, NWS's alerts; WMO codes, alert tiers).
- `components/sunsim_platform/` (`sunsim::platform`, module
  `sunsim.platform`) - everything ESP-IDF; `bulb_endpoint`,
  `fast_day_clock`, `format` and `wifi` are internal partitions, not exported:
  - `zigbee_adapter.cppm` - the only code using esp-zigbee-lib (stack,
    commissioning, discovery/bind/ping, button frames, bulb commands).
  - `clock.cppm`, `fast_day_clock.cppm` - heartbeat, program time, fast-day mode.
  - `internet_sync.cppm`, `wifi.cppm` - the internet jobs, daily (SNTP, the
    cloud forecast) and every 15 minutes (conditions, NWS alerts), over
    short Wi-Fi sessions; the saved networks.
  - `settings_store.cppm` - all NVS persistence (roles, settings, intensities,
    minimum levels).
  - `console.cppm` - the serial REPL; `status_display.cppm` - the LCD.
- `main/` - `main.cpp` (`App`: creates the bus and modules, platform wiring,
  app task; and `app_main`), menuconfig (`Kconfig.projbuild`), and the role manifest
  (`role_manifest.json` -> `role_manifest.h` via `gen_role_manifest.py`).
- `test/host/` - host tests for the pure layers, a file per area (see below).

## Host tests

The core logic is tested on your PC (no board, no ESP-IDF), wired exactly
as in the firmware and driven by events:

```
./sunsim test
# or by hand:
cmake -S test/host -B build-host -G Ninja && cmake --build build-host && ./build-host/sunsim_tests
```

C++ modules need Ninja and GCC 15 (ESP-IDF's `export.sh` puts its Ninja on
`PATH`).
