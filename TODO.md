# TODO

Open work, one item per future issue. The declarative-topology plan and the
decisions behind it are in ARCHITECTURE.md (§3 Value types, §13 Open
questions) and DESIGN.md §4 (The gate).

## Try the unmerged branches on the board

`feature/weather-overlays` and the stack on top of it, up to
`feature/declarative-topology`, pass the host tests and build, but none of
it has run on the device.

Check:

- [ ] Stored NVS settings and roles load (the keys are unchanged).
- [ ] InternetSync's `std::jthread`/semaphore boot sync works on ESP-IDF.
- [ ] Zigbee keeps working with a Wi-Fi session every 15 minutes.
- [ ] TLS to api.weather.gov works with the cert bundle.
- [ ] The weather colors look right on the real bulbs (`weather simulate`).
- [ ] Bulbs and the button are still discovered and bound (requests should
      be byte-for-byte as before).
- [ ] Button presses still switch modes; `status` shows the office's mode.
- [ ] `tune`, `tune save` and `tune off` still work.

## Grow lights

A zone for grow lights on their own schedule, independent of the office.

- [ ] A `Steady` look (`topology.cppm`): one color and level the whole time
      the gate is open; on a plug, just "on". Its `show()` goes in
      `daylight.cppm`.
- [ ] A `grow` `DaySpan` in `ProgramConfig`, with a `growhours` setting
      (a field plus a line in `kSettings`).
- [ ] A `Grow` group in `kLayout` (`Kind::Plug` or a bulb kind), and a grow
      zone in `kZones`: `Steady` look, `Window{.hours = &ProgramConfig::grow}`
      gate, `.modes = {Mode::Daylight, Mode::Off}`, no controls, no weather.
- [ ] Host tests: the grow zone follows its own hours, and a red alert or a
      hold in the office leaves it alone.

Done when: grow lights run their own photoperiod, configurable with
`config growhours`.

Later, maybe: a `SunCurve` that stretches real daylight to a target
photoperiod.

## Presence sensor

A human presence sensor replaces the office's active hours as its gate.
Per zone, either presence or the hours can take precedence.

- [ ] `Kind::Presence` with its caps. Discovery looks for an Occupancy
      Sensing (0x0406) server endpoint.
- [ ] ZigbeeAdapter: bind the sensor's Occupancy Sensing cluster to us,
      configure occupancy reporting, and post
      `OccupancyReported{role, occupied, at}`.
- [ ] DeviceRegistry: sensors count as heard on reports. Ping mains-powered
      sensors; don't ping sleepy, battery-powered ones.
- [ ] `PresenceModel` in core (a peer of `SkyModel`): per zone, any sensor
      occupied means present; after `hold` with no occupancy, absent (an
      `ev::Flow`). Post `PresenceChanged{zone, present, since}`.
- [ ] A `Presence` rule (`topology.cppm`): `sensors`, `hold` (a `Tunable`,
      with a `presencehold` setting), `fade_in`, `fade_out`, plus
      `opensOnly()` / `closesOnly()`. It gives no answer while the sensor is
      unknown, so the next rule (the hours) stands in.
- [ ] `core::openness` takes the zone's presence; fade from `since`.
- [ ] Compile-time checks: a `Presence` rule must name presence sensors, and
      every sensor must be used by some gate.
- [ ] Console `presence simulate on|off`, like `weather simulate`; `status`
      shows presence.
- [ ] Host tests for the precedence variants:
  - presence alone;
  - presence first, the hours as fallback;
  - the hours first (`closesOnly()`), then presence;
  - presence keeping the lights on after hours (`opensOnly()`).

Needs the board: the Zigbee reporting can't be tested on the host.

## Gate model: first answer wins, or All / Any?

Decide before presence is built (GitHub #13). Now a gate is a list where
the first rule with an answer decides, with `opensOnly()` / `closesOnly()`
for precedence: it can say anything, but has to be worked out to be read.
The alternative, `All{...}` / `Any{...}` (fades by min and max, an unknown
sensor neutral), reads as what you mean and can't have unreachable rules,
but can't say "presence decides, the hours only while the sensor is dead".

- [ ] Choose the model.
- [ ] If `All` / `Any`: what a lone `Presence` gate does while its sensor is
      unknown.

## More than one button

`kLayout` allows one button at most, because ZigbeeAdapter tells presses
apart by a single short address (`button_short_`, and `kButton` for the
role).

- [ ] Map each press's source address to its button's role, on both the
      APS path (`handleApsIndication`) and the bound-receiver path
      (`handleZclAction`).
- [ ] Drop the one-button `static_assert` in `types.cppm`.
- [ ] Console `press [<button>] single|double|hold`.

Needs the board.

## Rename `Mode::Daylight` to `Mode::Auto`

The mode means "run the zone's own looks and gate", which reads wrong for
grow lights. Rename it, keeping `daylight` as a console alias.

## The whole room in installation.cppm

`kLayout` (the groups and their kinds) still lives in `types.cppm`. One
obstacle is gone: `ProgramConfig` no longer depends on the layout, since
the weather lights moved into the zones. But `topology.cppm`'s checks use
the roles, which are derived from `kLayout`, and installation.cppm uses the
checks.

- [ ] Reorder `sunsim.ev`'s partitions so `kLayout` can sit in
      installation.cppm. Options: give the layout a partition of its own
      that installation.cppm re-exports, or make the checks independent of
      the roles until they're instantiated.
- [ ] Move the default peaks into the zone: `Shows{..., .peak = 90}`.
- [ ] Optional: replace the `Group` enum with names (a group is its place
      in `kLayout`).

## Status display: more than one zone

The display shows only the first zone's mode. Show every zone's, or the
office's plus a compact line for the others.
