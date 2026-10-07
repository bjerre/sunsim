# sunsim

An ESP32-C6 Zigbee coordinator that drives 4 Zigbee bulbs through a daylight
simulation and responds to one physical button controller:

- **3 "wall" bulbs** fade through a warm-to-cool, dim-to-bright curve
  between the real local sunrise and sunset, simulating daylight.
- **1 "work" bulb** stays bright and shifts cool to warm across the work
  hours (default 08:30-17:30: ~6000K in the morning to ~3000K by
  17:30, then holds warm).
- The wall bulbs sit east-to-west on the south wall (A east, C west): the
  one nearest the real sun's direction is brightest (`config sunshift`),
  and near sunrise/sunset they glow amber (`config amber`).
- All bulbs only run inside an **active window** (default Mon-Fri,
  08:00-19:00, 10 min fade at each edge) and are off outside it.
- **1 button controller** (any simple single-button Zigbee switch):
  - single press -> resume the daylight program
  - long press (Off) -> all lights off until pressed again
  - double press -> a "red alert" strobe effect on all 4 bulbs

sunsim forms and is the coordinator of its own small Zigbee network - it
does not join an existing hub's network. Built on
[esp-zigbee-sdk](https://github.com/espressif/esp-zigbee-sdk), targeting
ESP32-C6, written in C++20.

See [DESIGN.md](DESIGN.md) for architecture diagrams, how device addressing
and roles work, and the math behind the daylight curve.

## Building

`idf.py` only works in a shell where ESP-IDF's environment has been loaded
(once per terminal):

```
. ~/project/esp-idf/export.sh
```

Then, from this directory:

```
idf.py set-target esp32c6   # first time only - it regenerates sdkconfig
idf.py menuconfig           # "Sunsim Configuration": TZ, latitude/longitude, channel
idf.py -p /dev/ttyACM0 flash monitor   # builds too; Ctrl+] quits the monitor
```

The VS Code ESP-IDF extension's Build/Flash/Monitor buttons do the same
without the `export.sh` step.

## Active hours

```
sunsim> config                       # show current settings
sunsim> config hours 08:00-19:00
sunsim> config workhours 08:30-17:30   # work light's cool -> warm span
sunsim> config sunshift 25          # wall dimming away from the sun, % (0 = off)
sunsim> config amber on             # amber glow when the sun is < 6 deg up
sunsim> config fade 10               # minutes to fade in/out at each edge (0 = hard on/off)
sunsim> config days mon-fri          # or: all, or a list like mon,tue,thu
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
sunsim> intensity all 75
```

## Status display

The board's LCD (Waveshare ESP32-C6-GEEK, 1.14" ST7789) shows:

- the time, large, and the weekday ("sim" in fast-day mode)
- the mode: `daylight`, `idle` (outside active hours), `off` or `RED ALERT`
- today's day length, and the countdown to the next sunrise or sunset
- one dot per device: green = responding, amber = paired but silent,
  grey = not paired (labels A, B, C = wall bulbs, W = work, Btn = button)

The `status` console command prints the same information. Pins and
orientation are under menuconfig, "Sunsim Configuration" -> "Status
display"; "Rotate the picture 180 degrees" flips it if it's upside down.

## Wi-Fi (clock sync)

sunsim connects to Wi-Fi once at boot to set its clock over SNTP. Set the
network from the serial console; it's stored on the device and survives
reflashing:

```
sunsim> wifi "My Network" mypassword
sunsim> restart
```

`wifi` alone shows the current network, `wifi clear` forgets it. The Wi-Fi
SSID/password in menuconfig still work as a fallback when none is set from
the console.

Latitude/longitude (decimal degrees, North/East positive) drive the wall
bulbs' sunrise/sunset calculation - see
[DESIGN.md](DESIGN.md#5-how-location-is-provided). Left at the default
`0.0`/`0.0`, you get the equator/Greenwich meridian (sunrise/sunset close to
06:00/18:00 year-round).

## Pairing

Roles (`WallA`/`WallB`/`WallC`/`Work`/`Button`) are assigned from the serial
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

- `unpair <role>|all` - forget role assignments
- `open [seconds]` - reopen the network for joining (it opens for 180s at boot)
- `pair cancel` - disarm a pending `pair`
- `factory_reset` - forget all roles **and** the Zigbee network, then restart
  with a fresh network. The bulbs must then be factory-reset too, since
  they still remember the old network.

Roles and the Zigbee network both live in the `nvs` partition, so they survive
reflashing. `idf.py erase-flash` also wipes both.

Alternatively, devices can be pre-assigned by IEEE address in
[`main/role_manifest.json`](main/role_manifest.json) (used only when no `pair`
is armed). Use the exact `ext=` value from the log, not a hand-copied label.

You don't need all 5 devices present for sunsim to do something useful:
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
(`SunsimApp::handleApsIndication`). Buttons that instead send to the bound
receiver endpoint 2 still go through the older path: the On/Off attribute
change, with single vs. double press resolved by timing
(`SunsimApp::handleZclAction`, `ButtonController::onOffOffCommand`).

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

To bench-test the daylight curve without waiting for a real sunrise/sunset,
enable `SUNSIM_DEBUG_FAST_DAY` in menuconfig (and tune
`SUNSIM_DEBUG_DAY_SECONDS`, default 120). This:

- replaces the wall-clock time source with a synthetic clock that loops
  the active window (`config hours`, e.g. 08:00-19:00) every
  `SUNSIM_DEBUG_DAY_SECONDS` of real time, stepping through the active
  days only - the off hours and weekends are skipped
- updates lights every second instead of every minute, so the gradient is
  visibly animated
- skips the Wi-Fi/SNTP time sync at boot

It works with any subset of the 5 devices paired.

## Smooth transitions

Each periodic update sends the bulb's level/color-temperature commands with
a transition time matching the update interval (60s normally, 1s in debug
fast-day mode), so a bulb fades continuously from its current value to the
next target rather than snapping and holding. Explicit state changes that
should feel immediate instead - resuming after an override, a bulb
finishing discovery, a reboot - use a short fixed 2s transition
(`LightRig::snapRole`/`snapAllBulbs`) rather than waiting for the next slow
tick to catch up.

## Layout

- `main/daylight.*` - pure time -> `{on, level, color temperature}` curve math, no Zigbee dependency.
- `main/solar.*` - pure latitude/longitude/day-of-year -> sunrise/sunset calculation, no Zigbee dependency.
- `main/bulb_endpoint.*` - a Zigbee light endpoint's address, with `turnOn/turnOff/setLevel/setColorTemperature/setHueSaturation`.
- `main/role_manifest.json` - `ext_addr -> Role` table, filled in from device labels; `main/gen_role_manifest.py` turns it into a C++ header at build time.
- `main/roles_store.*` - persisted role -> device address/endpoint mapping.
- `main/pairing_console.*` - serial REPL: `pair`/`unpair`/`roles`/`open`/`status`/`config`/`intensity`/`press`/`wifi`/`restart`/`factory_reset`.
- `main/day_info.*` - day length and next sunrise/sunset, pure math like `solar.*`.
- `main/status.*` - one status snapshot shared by the `status` command and the display.
- `main/status_display.*` - LVGL screen on the on-board LCD.
- `main/schedule_settings.*` - persists the `config` settings (active hours, fade, days) in NVS.
- `main/light_rig.*` - owns the 5 paired devices, discovery/binding, and applies the daylight program.
- `main/button_controller.*` - single/double/long-press state machine, decoupled from Zigbee via callbacks.
- `main/wifi_time_sync.*` - one-shot Wi-Fi connect + SNTP sync, then Wi-Fi off (keeps the 802.15.4 radio clear).
- `main/fast_day_clock.h` - the debug-mode synthetic clock.
- `main/sunsim_app.*` - orchestrates everything; the Zigbee/esp_timer C callback trampolines live here.
