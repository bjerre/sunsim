import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import createSunsim from "../../sunsim_web_simulator.js";

const core = await createSunsim();
core._sunsim_init();

const state = () => JSON.parse(core.UTF8ToString(core._sunsim_state()));

let snapshot = state();
assert.equal(snapshot.lights.length, 8);
assert.deepEqual(snapshot.lights.map((light) => light.role), [
  "WallA", "WallB", "WallC", "WallD", "WallE", "Work", "Desk", "Grow",
]);
assert.deepEqual(snapshot.programFlow.map((zone) => zone.zone), [
  "office", "desk-downlight", "bookcase-grow-light",
]);
assert.equal(snapshot.programFlow[0].mode, "daylight");
assert.equal(snapshot.programFlow[0].gateOpen, true);
assert.equal(snapshot.programFlow[0].gateApplies, true);
assert.deepEqual(snapshot.programFlow[0].availableModes, [
  { mode: "daylight", buttonPress: "single" },
  { mode: "off", buttonPress: "hold" },
  { mode: "RED ALERT", buttonPress: "double" },
  { mode: "tuning", buttonPress: null },
]);
assert.deepEqual(snapshot.programFlow[1].availableModes, [{ mode: "daylight", buttonPress: null }]);
assert.deepEqual(snapshot.programFlow[2].availableModes, [{ mode: "daylight", buttonPress: null }]);
assert.deepEqual(snapshot.programFlow[0].members.map((member) => member.role), [
  "WallA", "WallB", "WallC", "WallD", "WallE", "Work",
]);
assert.deepEqual(snapshot.programFlow[0].members.map((member) => member.behavior), [
  "sun curve", "sun curve", "sun curve", "sun curve", "sun curve", "work curve",
]);
assert.equal(snapshot.programFlow[1].members[0].behavior, "on/off");
const wallCurve = snapshot.programFlow[0].members[0].curve;
assert.equal(wallCurve.length, 97);
assert.equal(wallCurve[0].minute, 0);
assert.equal(wallCurve[96].minute, 1439);
assert.ok(wallCurve.some((sample) => sample.on && sample.level > 0));
const clearWallNoon = wallCurve[48].level;
const clearWorkNoon = snapshot.programFlow[0].members[5].curve[48].level;
core._sunsim_weather(0, 100);
snapshot = state();
assert.equal(snapshot.sky, "cloud 100%");
assert.ok(snapshot.programFlow[0].members[0].curve[48].level < clearWallNoon);
assert.equal(snapshot.programFlow[0].members[5].curve[48].level, clearWorkNoon);
const deskCurve = snapshot.programFlow[1].members[0].curve;
assert.equal(deskCurve[31].minute, 465);
assert.equal(deskCurve[31].on, false);
assert.equal(deskCurve[32].minute, 480);
assert.equal(deskCurve[32].on, true);
assert.equal(deskCurve[75].on, true);
assert.equal(deskCurve[76].minute, 1140);
assert.equal(deskCurve[76].on, false);
const growCurve = snapshot.programFlow[2].members[0].curve;
assert.equal(growCurve[14].on, false);
assert.equal(growCurve[15].minute, 225);
assert.equal(growCurve[15].on, true);
assert.equal(growCurve[34].on, true);
assert.equal(growCurve[35].minute, 525);
assert.equal(growCurve[35].on, false);
assert.equal(snapshot.devices.length, 9);
assert.equal(snapshot.weekday, "sim Tue");
assert.equal(snapshot.mode, "daylight");
assert.match(snapshot.sunrise, /^Rise \d{2}:\d{2}$/);
assert.match(snapshot.sunset, /^Set \d{2}:\d{2}$/);
assert.ok(snapshot.cloudPercent === null ||
  (Number.isInteger(snapshot.cloudPercent) && snapshot.cloudPercent >= 0 && snapshot.cloudPercent <= 100));
assert.equal(snapshot.precipitation, "none");
assert.ok(snapshot.lights.some((light) => light.on));
assert.equal(snapshot.lights.find((light) => light.role === "Desk").on, true);
assert.equal(snapshot.lights.find((light) => light.role === "Grow").on, false);

assert.equal(core._sunsim_set_clock(2025, 1, 15, 5, 0, -420), 1);
snapshot = state();
assert.equal(snapshot.time, "05:00");
assert.equal(snapshot.utcOffsetMinutes, -420);
assert.equal(snapshot.simDate, "2025-01-15");
assert.equal(snapshot.programFlow[1].gateOpen, false);
assert.equal(snapshot.programFlow[2].gateOpen, true);
assert.equal(snapshot.lights.find((light) => light.role === "Desk").on, false);
assert.equal(snapshot.lights.find((light) => light.role === "Grow").on, true);
core._sunsim_press(1);
snapshot = state();
assert.equal(snapshot.programFlow[0].mode, "RED ALERT");
assert.equal(snapshot.programFlow[0].gateOpen, false);
assert.equal(snapshot.programFlow[0].gateApplies, false);
assert.ok(snapshot.lights.filter((light) => light.role.startsWith("Wall") || light.role === "Work")
  .every((light) => light.on));
const redAlertLevel = snapshot.lights.find((light) => light.role === "WallA").level;
assert.ok(redAlertLevel > 0);
assert.equal(snapshot.lights.find((light) => light.role === "WallA").weatherEffect, "none");
core._sunsim_advance(1);
assert.equal(core._sunsim_set_clock(2025, 1, 15, 5, 0, -420), 1);
snapshot = state();
assert.notEqual(snapshot.lights.find((light) => light.role === "WallA").level, redAlertLevel);
core._sunsim_press(0);
snapshot = state();
assert.equal(snapshot.programFlow[0].mode, "daylight");
assert.equal(snapshot.programFlow[0].gateApplies, true);
assert.ok(snapshot.lights.filter((light) => light.role.startsWith("Wall") || light.role === "Work")
  .every((light) => !light.on));
assert.ok(Number.isFinite(snapshot.sun.azimuthDeg));
assert.ok(Number.isFinite(snapshot.sun.elevationDeg));
assert.equal(core._sunsim_set_clock(2025, 2, 30, 12, 0, -420), 0);

assert.equal(core._sunsim_set_location(0, 0), 1);
assert.equal(core._sunsim_set_clock(2026, 10, 25, 5, 0, 0), 1);
snapshot = state();
assert.equal(snapshot.time, "05:00");
assert.equal(snapshot.utcOffsetMinutes, 0);
assert.match(snapshot.nextEvent, /^Rise .* 05:/);

assert.equal(core._sunsim_set_location(47.6, -122.3), 1);
snapshot = state();
assert.equal(snapshot.location.latitude, 47.6);
assert.equal(snapshot.location.longitude, -122.3);
assert.ok(snapshot.sun.elevationDeg < 0);
assert.equal(core._sunsim_set_clock(2025, 1, 15, 12, 0, -480), 1);
snapshot = state();
assert.ok(snapshot.sun.elevationDeg > 0);
assert.equal(core._sunsim_set_clock(2025, 1, 15, 12, 0, -420), 1);
snapshot = state();
assert.equal(snapshot.time, "12:00");
assert.equal(snapshot.lights.find((light) => light.role === "Work").on, true);
assert.equal(core._sunsim_set_clock(2025, 1, 18, 12, 0, -420), 1);
snapshot = state();
assert.equal(snapshot.time, "12:00");
assert.equal(snapshot.lights.find((light) => light.role === "Work").on, false);
assert.equal(snapshot.lights.find((light) => light.role === "Desk").on, true);
assert.equal(core._sunsim_set_clock(2025, 1, 15, 10, 0, -480), 1);
assert.equal(core._sunsim_set_location(91, -122.3), 0);

core._sunsim_press(2);
snapshot = state();
assert.equal(snapshot.mode, "off");
assert.equal(snapshot.programFlow[0].mode, "off");
assert.ok(snapshot.lights.filter((light) => light.role.startsWith("Wall") || light.role === "Work")
  .every((light) => !light.on));
assert.equal(snapshot.lights.find((light) => light.role === "Desk").on, true);

core._sunsim_press(0);
core._sunsim_set_minute(600);
snapshot = state();
assert.equal(snapshot.simMinute, 600);
assert.ok(snapshot.lights.some((light) => light.on));

core._sunsim_weather(2, 90);
snapshot = state();
assert.equal(snapshot.sky, "rain");
const rainOutput = snapshot.lights.find((light) => light.role === "WallC");
assert.equal(rainOutput.color.mode, "hue");
assert.equal(rainOutput.weatherEffect, "rain");
assert.ok(["bright", "dim"].includes(rainOutput.pulsePhase));
const rainPhase = rainOutput.pulsePhase;
core._sunsim_advance(2);
core._sunsim_set_minute(600);
snapshot = state();
assert.notEqual(snapshot.lights.find((light) => light.role === "WallC").pulsePhase, rainPhase);
core._sunsim_alert(3);
snapshot = state();
assert.equal(snapshot.sky, "warning");
assert.equal(snapshot.lights.find((light) => light.role === "WallB").weatherEffect, "warning");
assert.equal(snapshot.lights.find((light) => light.role === "WallD").weatherEffect, "warning");

const roleIndex = new Map([
  ["WallA", 0], ["WallB", 1], ["WallC", 2], ["WallD", 3], ["WallE", 4],
  ["Work", 5], ["Desk", 6], ["Grow", 7], ["Button", 8],
]);
const lookIndex = new Map([["sun-curve", 0], ["work-curve", 1], ["on-off", 2]]);
const invalidProfile = await createSunsim();
assert.equal(invalidProfile._sunsim_profile_begin(1), 1);
const invalidNameBytes = invalidProfile.lengthBytesUTF8("unassigned-weather") + 1;
const invalidName = invalidProfile._malloc(invalidNameBytes);
try {
  invalidProfile.stringToUTF8("unassigned-weather", invalidName, invalidNameBytes);
  assert.equal(invalidProfile._sunsim_profile_zone(0, invalidName, 0, 1440), 1);
} finally {
  invalidProfile._free(invalidName);
}
assert.equal(invalidProfile._sunsim_profile_weather(0, roleIndex.get("WallC"), 0), 0);

async function startProfile(path) {
  const profile = JSON.parse(readFileSync(new URL(path, import.meta.url), "utf8"));
  const instance = await createSunsim();
  assert.equal(instance._sunsim_profile_begin(profile.zones.length), 1);
  for (const [zoneIndex, zone] of profile.zones.entries()) {
    const bytes = instance.lengthBytesUTF8(zone.name) + 1;
    const pointer = instance._malloc(bytes);
    assert.notEqual(pointer, 0);
    try {
      instance.stringToUTF8(zone.name, pointer, bytes);
      assert.equal(
        instance._sunsim_profile_zone(zoneIndex, pointer, zone.startMinute, zone.endMinute),
        1,
      );
    } finally {
      instance._free(pointer);
    }
    for (const member of zone.members) {
      assert.equal(
        instance._sunsim_profile_member(
          zoneIndex,
          roleIndex.get(member.role),
          lookIndex.get(member.look),
          member.peakPercent ?? 100,
        ),
        1,
      );
    }
    if (zone.buttonRole) {
      assert.equal(
        instance._sunsim_profile_button(zoneIndex, roleIndex.get(zone.buttonRole)),
        1,
      );
    }
    for (const [effect, roles] of [
      [0, zone.weather?.precipitation ?? []],
      [1, zone.weather?.alerts ?? []],
    ]) {
      for (const role of roles) {
        assert.equal(
          instance._sunsim_profile_weather(zoneIndex, roleIndex.get(role), effect),
          1,
        );
      }
    }
  }
  assert.equal(instance._sunsim_profile_finish(), 1);
  instance._sunsim_init();
  return { instance, state: () => JSON.parse(instance.UTF8ToString(instance._sunsim_state())) };
}

const officeProfile = await startProfile("../../web/public/profiles/office.json");
snapshot = officeProfile.state();
assert.deepEqual(snapshot.programFlow.map((zone) => zone.zone), [
  "office", "desk-downlight", "bookcase-grow-light",
]);
assert.equal(snapshot.programFlow[0].members.length, 6);
officeProfile.instance._sunsim_set_minute(450);
assert.equal(officeProfile.state().lights.find((light) => light.role === "Desk").on, false);

const studioProfile = await startProfile("../../web/public/profiles/studio.json");
snapshot = studioProfile.state();
assert.deepEqual(snapshot.programFlow.map((zone) => zone.zone), ["studio", "plants"]);
assert.deepEqual(snapshot.programFlow[0].members.map((member) => member.role), [
  "WallA", "WallB", "WallC", "WallD", "WallE", "Work", "Desk",
]);
assert.deepEqual(snapshot.programFlow[0].availableModes.map((mode) => mode.buttonPress), [
  "single", "hold", "double", null,
]);
studioProfile.instance._sunsim_press(1);
assert.equal(studioProfile.state().programFlow[0].mode, "RED ALERT");
studioProfile.instance._sunsim_press(0);
studioProfile.instance._sunsim_set_minute(450);
assert.equal(studioProfile.state().lights.find((light) => light.role === "Desk").on, true);
assert.equal(studioProfile.state().lights.find((light) => light.role === "Grow").on, true);

console.log("WASM browser simulation smoke test passed");
