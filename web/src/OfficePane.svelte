<script lang="ts">
  import type { SimLight, SimulatorState, WallRole } from "../simulator.js";
  import { torchiereView, wallLightView } from "../presentation.js";

  let { snapshot }: { snapshot: SimulatorState } = $props();
  const wallRoles: WallRole[] = ["WallA", "WallB", "WallC", "WallD", "WallE"];
  let wallLights = $derived(
    wallRoles.flatMap((role) => {
      const light = snapshot.lights.find((candidate) => candidate.role === role);
      return light ? [light] : [];
    }),
  );
  let workLight = $derived(snapshot.lights.find((light) => light.role === "Work"));
  let workView = $derived(workLight
    ? torchiereView(workLight, snapshot.mode)
    : { color: "#f0ce82", opacity: 0, status: "Waiting for program", shadeBackground: "", filter: "" });
  let deskLight = $derived(snapshot.lights.find((light) => light.role === "Desk"));
  let growLight = $derived(snapshot.lights.find((light) => light.role === "Grow"));
  let onlineCount = $derived(snapshot.devices.filter((device) => device.link === "online").length);
  let activeCount = $derived(snapshot.lights.filter((light) => light.on).length);
</script>

<section class="office panel" aria-label="Virtual office lights">
  <div class="section-heading">
    <div><p class="eyebrow">LIVE SIMULATION</p><h2>Office</h2></div>
    <span class="badge" data-tone={snapshot.modeTone}>{snapshot.mode || "Idle"}</span>
  </div>

  <div class="room">
    <div class="wall">
      <div class="wall-label">WINDOW LIGHT · WALL A → E · EAST → WEST</div>
      <div class="wall-lights">
        {#each wallLights as light (light.role)}
          {@const view = wallLightView(light)}
          <div class="bulb-unit">
            <span
              class="wall-uplight"
              style={`--wall-uplight-color: ${view.color}; opacity: ${view.uplightOpacity};`}
            ></span>
            <span
              class="bulb"
              style={`background: ${view.bulbBackground}; filter: ${view.bulbFilter}; box-shadow: ${view.bulbBoxShadow};`}
            ></span>
            <span>{light.role}</span>
            <small style={`color: ${light.on ? view.color : ""};`}>{view.label}</small>
          </div>
        {/each}
      </div>
    </div>
    <div class="torchiere" aria-hidden="true" style={`filter: ${workView.filter};`}>
      <span class="torchiere-glow" style={`opacity: ${workView.opacity}; --torchiere-color: ${workView.color};`}></span>
      <span class="torchiere-shade" style={`background: ${workView.shadeBackground};`}></span>
      <span class="torchiere-stem"></span><span class="torchiere-base"></span>
    </div>
    <div class="torchiere-caption"><strong>Room torchiere</strong><span>{workView.status}</span></div>
    <div
      class="bookcase-scene"
      data-on={growLight?.on ?? false}
      aria-label="Bookcase with scheduled grow light"
    >
      <span class="plant-glow"></span>
      <span class="grow-lamp"></span>
      <span class="plant-leaf leaf-one"></span><span class="plant-leaf leaf-two"></span>
      <span class="plant-leaf leaf-three"></span><span class="plant-leaf leaf-four"></span>
      <span class="plant-pot"></span>
      <span class="bookcase-shelf shelf-top"></span><span class="bookcase-shelf shelf-bottom"></span>
      <span class="bookcase-caption">
        <strong>Bookcase · grow light</strong>
        <small>{growLight?.on ? "ON · 03:45–08:45" : "OFF · 03:45–08:45"}</small>
      </span>
    </div>
    <div
      class="desk-scene"
      data-on={deskLight?.on ?? false}
      aria-label="Desk with scheduled downlight"
    >
      <span class="desk-downlight-beam"></span>
      <span class="desk-lamp-post"></span><span class="desk-lamp-arm"></span><span class="desk-lamp-head"></span>
      <span class="desk-surface"></span><span class="desk-leg leg-left"></span><span class="desk-leg leg-right"></span>
      <span class="desk-caption">
        <strong>Desk downlight</strong>
        <small>{deskLight?.on ? "ON · 08:00–19:00" : "OFF · 08:00–19:00"}</small>
      </span>
    </div>
    <div class="room-footer"><span>SIMULATED DEVICES</span><span>{onlineCount} / {snapshot.devices.length} online</span></div>
  </div>
  <p class="hint">
    The torchiere follows its weekday work-hours program. The desk downlight and bookcase grow light are on independent daily plug schedules and stay schedule-controlled when the office button is used.
  </p>
  <div class="light-legend">
    <span><i class="legend-on"></i>On</span>
    <span><i class="legend-off"></i>Off</span>
    <span>{snapshot.sky ? `Sky: ${snapshot.sky}` : "Clear sky"}</span>
    <span>{activeCount} / {snapshot.lights.length} lights on</span>
  </div>
</section>
