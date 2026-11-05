<script lang="ts">
  import type { SimulatorState } from "../simulator.js";
  import { windowView } from "../presentation.js";

  let { snapshot }: { snapshot: SimulatorState } = $props();
  let view = $derived(windowView(snapshot));
</script>

<section class="window-panel panel" aria-label="South-facing window simulation">
  <div class="section-heading compact">
    <div><p class="eyebrow">SIMULATED OUTSIDE</p><h2>South-facing window</h2></div>
    <span class="badge">{view.label}</span>
  </div>
  <div
    class:sun-visible={view.visible}
    class="window-scene"
    style={`--window-brightness: ${view.brightness}; --window-tint: ${view.tint};`}
  >
    <span></span><span></span><span></span><span></span>
    <span class="window-sun" aria-hidden="true" style={`left: ${view.left}; top: ${view.top};`}></span>
  </div>
  <p class="hint">Sun position follows the simulated date, time, and location. Glass tint follows the WallA–E daylight colors.</p>
</section>
