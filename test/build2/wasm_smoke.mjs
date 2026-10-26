import assert from "node:assert/strict";
import createSunsim from "../../sunsim_web_simulator.js";

const core = await createSunsim();
core._sunsim_init();

const state = () => JSON.parse(core.UTF8ToString(core._sunsim_state()));

let snapshot = state();
assert.equal(snapshot.lights.length, 8);
assert.deepEqual(snapshot.lights.map((light) => light.role), [
  "WallA", "WallB", "WallC", "WallD", "WallE", "Work", "Desk", "Grow",
]);
assert.equal(snapshot.devices.length, 9);
assert.equal(snapshot.weekday, "sim Tue");
assert.equal(snapshot.mode, "daylight");
assert.ok(snapshot.lights.some((light) => light.on));
assert.equal(snapshot.lights.find((light) => light.role === "Desk").on, true);
assert.equal(snapshot.lights.find((light) => light.role === "Grow").on, false);

assert.equal(core._sunsim_set_clock(2025, 1, 15, 5, 0, -420), 1);
snapshot = state();
assert.equal(snapshot.time, "05:00");
assert.equal(snapshot.utcOffsetMinutes, -420);
assert.equal(snapshot.simDate, "2025-01-15");
assert.equal(snapshot.lights.find((light) => light.role === "Desk").on, false);
assert.equal(snapshot.lights.find((light) => light.role === "Grow").on, true);
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
assert.ok(snapshot.lights.filter((light) => light.role.startsWith("Wall") || light.role === "Work")
  .every((light) => !light.on));
assert.equal(snapshot.lights.find((light) => light.role === "Desk").on, true);

core._sunsim_press(0);
core._sunsim_set_minute(600);
snapshot = state();
assert.equal(snapshot.simMinute, 600);
assert.ok(snapshot.lights.some((light) => light.on));

core._sunsim_weather(2, 90);
core._sunsim_alert(3);
snapshot = state();
assert.equal(snapshot.sky, "warning");

console.log("WASM browser simulation smoke test passed");
