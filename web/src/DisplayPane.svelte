<script lang="ts">
  import type { DeviceRole, SimulatorState } from "../simulator.js";

  let { snapshot }: { snapshot: SimulatorState } = $props();
  let view = $state<"sky" | "status">("sky");
  let activeCount = $derived(snapshot.lights.filter((light) => light.on).length);
  let sun = $derived.by(() => {
    const azimuth = snapshot.sun.azimuthDeg * Math.PI / 180;
    return {
      x: 120 - Math.sin(azimuth) * 101,
      y: Math.max(68, Math.min(111, 102 - snapshot.sun.elevationDeg * 0.4)),
      visible: snapshot.time !== "--:--",
    };
  });
  let skyStops = $derived.by(() => {
    const elevation = snapshot.sun.elevationDeg;
    if (!Number.isFinite(elevation) || elevation < -6) {
      return ["#050d22", "#091733", "#112449", "#182c50", "#1d3152"];
    }
    if (elevation < 0) return ["#182044", "#343b68", "#765778", "#d8836c", "#f5a36d"];
    if (elevation < 12) return ["#425b91", "#7b71a6", "#c98289", "#f3a274", "#ffd08a"];
    return ["#2777ae", "#3489bd", "#55a5ce", "#7bbbd2", "#a6d0d3"];
  });
  let cloudVisible = $derived(
    snapshot.precipitation !== "none" || (snapshot.cloudPercent !== null && snapshot.cloudPercent >= 25),
  );
  let cloudOpacity = $derived(
    Math.max(0.4, Math.min(1, 0.4 + (snapshot.cloudPercent ?? 100) / 170)),
  );
  let precipitationVisible = $derived(snapshot.precipitation !== "none");

  const linkGroups: { label: string; labelX: number; firstDotX: number; roles: DeviceRole[] }[] = [
    { label: "D", labelX: 7, firstDotX: 23, roles: ["WallA", "WallB", "WallC", "WallD", "WallE"] },
    { label: "W", labelX: 83, firstDotX: 99, roles: ["Work"] },
    { label: "L", labelX: 117, firstDotX: 133, roles: ["Desk"] },
    { label: "G", labelX: 151, firstDotX: 167, roles: ["Grow"] },
    { label: "B", labelX: 187, firstDotX: 203, roles: ["Button"] },
  ];

  function toneColor(tone: string): string {
    const colors: Record<string, string> = {
      muted: "#b4c0c5",
      daylight: "#ffe0a0",
      tuning: "#9ee2ff",
      advisory: "#ffe277",
      watch: "#ffad57",
      warning: "#ff746b",
      sky: "#c5dce8",
    };
    return colors[tone] ?? "#ffffff";
  }

  function deviceLinkColor(role: DeviceRole): string {
    const link = snapshot.devices.find((device) => device.role === role)?.link;
    if (link === "online") return "#4ee095";
    if (link === "silent") return "#f1b65c";
    return "#46515a";
  }
</script>

<section class="display panel" aria-label="Simulated ESP32 display">
  <div class="section-heading compact">
    <div><p class="eyebrow">DEVICE DISPLAY</p><h2>ESP32-C6-GEEK</h2></div>
    <div class="display-controls">
      <div class="display-toggle" role="group" aria-label="Display style">
        <button type="button" aria-pressed={view === "sky"} onclick={() => (view = "sky")}>Sky</button>
        <button type="button" aria-pressed={view === "status"} onclick={() => (view = "status")}>Status</button>
      </div>
      <span class="display-led"></span>
    </div>
  </div>

  {#if view === "sky"}
    <div class="lcd-sky-frame">
      <svg
        class="lcd-sky-art"
        viewBox="0 0 240 135"
        role="img"
        aria-label={`Simulated LCD sky at ${snapshot.time}, ${snapshot.sky || "clear"}`}
      >
        <defs>
          <linearGradient id="lcd-sky-gradient" x1="0" y1="0" x2="0" y2="1">
            {#each skyStops as color, index}
              <stop offset={`${index * 25}%`} stop-color={color} />
            {/each}
          </linearGradient>
          <filter id="lcd-sun-glow" x="-100%" y="-100%" width="300%" height="300%">
            <feGaussianBlur stdDeviation="5" />
          </filter>
        </defs>

        <rect width="240" height="135" fill="url(#lcd-sky-gradient)" />
        <path d="M18 102 Q120 44 222 102" fill="none" stroke="#ffe2ad" stroke-dasharray="1 7" stroke-linecap="round" opacity=".7" />

        {#if cloudVisible}
          <g fill="#d6e0e4" opacity={cloudOpacity}>
            <rect x="142" y="75" width="35" height="8" rx="4" />
            <circle cx="155" cy="75" r="8" />
            <circle cx="168" cy="73" r="10" />
            <circle cx="177" cy="76" r="7" />
          </g>
        {/if}

        {#if precipitationVisible}
          <g stroke={snapshot.precipitation === "snow" ? "#f4f7ff" : "#b8ebff"} stroke-width="2" stroke-linecap="round">
            {#if snapshot.precipitation === "snow"}
              <circle cx="153" cy="88" r="2" fill="#f4f7ff" stroke="none" />
              <circle cx="164" cy="91" r="2" fill="#f4f7ff" stroke="none" />
              <circle cx="175" cy="87" r="2" fill="#f4f7ff" stroke="none" />
            {:else}
              <path d="M153 85v6 M164 87v6 M175 84v6" />
            {/if}
          </g>
        {/if}

        {#if sun.visible}
          <circle cx={sun.x} cy={sun.y} r="13" fill="#ffd079" opacity=".65" filter="url(#lcd-sun-glow)" />
          <circle cx={sun.x} cy={sun.y} r="6" fill={snapshot.sun.elevationDeg < -1 ? "#c4b8a4" : "#ffe19a"} />
        {/if}

        <g fill="#172a43">
          <rect x="8" y="117" width="24" height="18" />
          <rect x="39" y="122" width="19" height="13" />
          <rect x="66" y="111" width="31" height="24" />
          <rect x="105" y="119" width="23" height="16" />
          <rect x="137" y="114" width="28" height="21" />
          <rect x="174" y="120" width="48" height="15" />
        </g>
        <path d="M0 103H240" stroke="#ffe0ad" stroke-width="1" opacity=".55" />
        <rect x="46" y="103" width="148" height="15" rx="7" fill="#0c1929" opacity=".75" />
        <rect x="0" y="118" width="240" height="17" fill="#081321" />

        <text class="lcd-art-time" x="6" y="40">{snapshot.time}</text>
        <text class="lcd-art-meta" x="234" y="14" text-anchor="end">{snapshot.weekday || "SIMULATED"}</text>
        <text class="lcd-art-meta" x="234" y="31" text-anchor="end" fill={toneColor(snapshot.modeTone)}>{snapshot.mode || "idle"}</text>
        <text class="lcd-art-meta" x="234" y="48" text-anchor="end" fill={toneColor(snapshot.skyTone)}>{snapshot.sky || "clear"}</text>
        <text class="lcd-art-small" x="7" y="64">{snapshot.dayLength}</text>
        <text class="lcd-art-small" x="6" y="98">{snapshot.sunrise}</text>
        <text class="lcd-art-small" x="234" y="98" text-anchor="end">{snapshot.sunset}</text>
        <text class="lcd-art-small lcd-art-event" x="120" y="113" text-anchor="middle">{snapshot.nextEvent}</text>

        {#each linkGroups as group}
          <text class="lcd-art-link-label" x={group.labelX} y="131">{group.label}</text>
          {#each group.roles as role, index}
            <circle cx={group.firstDotX + index * 10} cy="127" r="4" fill={deviceLinkColor(role)} />
          {/each}
        {/each}
      </svg>
    </div>
  {:else}
    <div class="lcd">
      <div class="lcd-top"><span>{snapshot.weekday || "SIMULATED"}</span><span class="lcd-time">{snapshot.time}</span></div>
      <div class="lcd-mode">{snapshot.mode || "Waiting"}</div>
      <div class="lcd-rule"></div>
      <div class="lcd-line"><span>DAYLIGHT</span><strong>{snapshot.dayLength}</strong></div>
      <div class="lcd-line"><span>NEXT EVENT</span><strong>{snapshot.nextEvent}</strong></div>
      <div class="lcd-line"><span>SKY</span><strong>{snapshot.sky || "clear"}</strong></div>
      <div class="lcd-bottom"><span>LIGHTS</span><strong>{activeCount} / {snapshot.lights.length}</strong></div>
    </div>
  {/if}
</section>
