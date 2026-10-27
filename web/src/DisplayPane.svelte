<script lang="ts">
  import type { SimulatorState } from "../simulator.js";

  let { snapshot }: { snapshot: SimulatorState } = $props();
  let activeCount = $derived(snapshot.lights.filter((light) => light.on).length);
</script>

<section class="display panel" aria-label="Simulated ESP32 display">
  <div class="section-heading compact">
    <div><p class="eyebrow">DEVICE DISPLAY</p><h2>ESP32-C6-GEEK</h2></div>
    <span class="display-led"></span>
  </div>
  <div class="lcd">
    <div class="lcd-top"><span>{snapshot.weekday || "SIMULATED"}</span><span class="lcd-time">{snapshot.time}</span></div>
    <div class="lcd-mode">{snapshot.mode || "Waiting"}</div>
    <div class="lcd-rule"></div>
    <div class="lcd-line"><span>DAYLIGHT</span><strong>{snapshot.dayLength}</strong></div>
    <div class="lcd-line"><span>NEXT EVENT</span><strong>{snapshot.nextEvent}</strong></div>
    <div class="lcd-line"><span>SKY</span><strong>{snapshot.sky || "clear"}</strong></div>
    <div class="lcd-bottom"><span>LIGHTS</span><strong>{activeCount} / {snapshot.lights.length}</strong></div>
  </div>
</section>
