<script lang="ts">
  import { colorCss } from "../presentation.js";
  import type { ButtonPress, LightRole, ProgramFlowMember, ProgramFlowMode, SimulatorState } from "../simulator.js";

  let {
    snapshot,
    onButtonPress,
  }: {
    snapshot: SimulatorState;
    onButtonPress: (kind: ButtonPress) => void;
  } = $props();
  let selectedRole = $state<LightRole>("WallA");
  const flowMembers = $derived(snapshot.programFlow.flatMap((zone) => zone.members));
  const selectedMember = $derived(
    flowMembers.find((member) => member.role === selectedRole) ?? flowMembers[0],
  );
  const selectedZone = $derived(
    snapshot.programFlow.find((zone) =>
      zone.members.some((member) => member.role === selectedMember?.role),
    ),
  );

  function zoneTitle(zone: string): string {
    const titles: Record<string, string> = {
      office: "Office lights",
      "desk-downlight": "Desk downlight",
      "bookcase-grow-light": "Bookcase grow light",
    };
    return titles[zone] ?? zone;
  }

  function programTitle(mode: string): string {
    const titles: Record<string, string> = {
      daylight: "Daylight",
      off: "Manual off",
      "RED ALERT": "Red alert",
      tuning: "Tuning",
    };
    return titles[mode] ?? mode;
  }

  function gateStatus(zone: SimulatorState["programFlow"][number]): string {
    if (zone.gateApplies) return zone.gateOpen ? "Open" : "Closed";
    return zone.mode === "off" ? "Forced off" : "Bypassed";
  }

  function activateMode(choice: ProgramFlowMode): void {
    if (choice.buttonPress === null) return;
    const press: Record<NonNullable<ProgramFlowMode["buttonPress"]>, ButtonPress> = {
      single: 0,
      double: 1,
      hold: 2,
    };
    onButtonPress(press[choice.buttonPress]);
  }

  function lightIsOn(role: LightRole): boolean {
    return lightFor(role)?.on ?? false;
  }

  function lightLevel(role: LightRole): number {
    return lightFor(role)?.level ?? 0;
  }

  function lightFor(role: LightRole): SimulatorState["lights"][number] | undefined {
    return snapshot.lights.find((light) => light.role === role);
  }

  function pulsePhase(zone: SimulatorState["programFlow"][number]): "HIGH" | "LOW" | null {
    if (zone.mode !== "RED ALERT") return null;
    const member = zone.members[0];
    if (!member) return null;
    return lightLevel(member.role) >= 128 ? "HIGH" : "LOW";
  }

  function pulseLevelPercent(role: LightRole): number {
    return Math.round((Math.max(0, Math.min(254, lightLevel(role))) / 254) * 100);
  }

  function weatherOverlays(zone: SimulatorState["programFlow"][number]) {
    return zone.members.flatMap((member) => {
      const light = lightFor(member.role);
      if (!light || light.weatherEffect === "none") return [];
      return [{ role: member.role, effect: light.weatherEffect, phase: light.pulsePhase }];
    });
  }

  function overlayTitle(effect: string): string {
    if (effect === "advisory" || effect === "watch" || effect === "warning") {
      return `${effect} alert`;
    }
    return `${effect} pulse`;
  }

  function xFor(minute: number): number {
    return 42 + (minute / 1439) * 584;
  }

  function yFor(sample: ProgramFlowMember["curve"][number]): number {
    const level = sample.on ? Math.max(0, Math.min(254, sample.level)) : 0;
    return 126 - (level / 254) * 96;
  }

  function curvePath(member: ProgramFlowMember): string {
    const first = member.curve[0];
    if (!first) return "";
    let path = `M ${xFor(first.minute)} ${yFor(first)}`;
    for (const sample of member.curve.slice(1)) {
      if (member.behavior === "on/off") {
        path += ` H ${xFor(sample.minute)} V ${yFor(sample)}`;
      } else {
        path += ` L ${xFor(sample.minute)} ${yFor(sample)}`;
      }
    }
    return path;
  }
</script>

<section class="program-flow panel" aria-label="Program flow by zone">
  <div class="section-heading compact">
    <div>
      <p class="eyebrow">CORE → OUTPUT</p>
      <h2>Program flow</h2>
    </div>
    <span class="badge">LIVE</span>
  </div>
  <p class="hint flow-intro">Follow each zone from its selected program through its schedule gate and any weather override to the lights and plugs.</p>

  <div class="flow-zones">
    {#each snapshot.programFlow as zone (zone.zone)}
      {@const pulse = pulsePhase(zone)}
      {@const overlays = weatherOverlays(zone)}
      <article class="flow-zone">
        <div class="flow-zone-heading">
          <h3>{zoneTitle(zone.zone)}</h3>
          <span class="zone-id">{zone.zone}</span>
        </div>
        <div class="flow-path">
          <div class="flow-stage program-stage">
            <span>ACTIVE PROGRAM</span>
            <strong>{programTitle(zone.mode)}</strong>
            {#if pulse}
              <small class="flow-program-pulse" data-phase={pulse}>PULSE · {pulse}</small>
            {/if}
            <div class="flow-mode-list" role="group" aria-label="Programs supported by this zone">
              {#each zone.availableModes as choice (choice.mode)}
                <button
                  class="flow-mode-chip"
                  type="button"
                  data-active={zone.mode === choice.mode}
                  data-mode={choice.mode}
                  data-pulse={zone.mode === choice.mode ? pulse ?? undefined : undefined}
                  aria-pressed={zone.mode === choice.mode}
                  aria-label={choice.buttonPress === null
                    ? `${programTitle(choice.mode)} mode is not mapped to a room button`
                    : `Select ${programTitle(choice.mode)} via ${choice.buttonPress} button press`}
                  disabled={choice.buttonPress === null || zone.mode === choice.mode}
                  onclick={() => activateMode(choice)}
                >
                  {programTitle(choice.mode)}
                </button>
              {/each}
            </div>
          </div>
          <span class="flow-arrow" aria-hidden="true">→</span>
          <div
            class="flow-stage gate-stage"
            data-open={zone.gateApplies && zone.gateOpen}
            data-applies={zone.gateApplies}
            data-mode={zone.mode}
          >
            <span>SCHEDULE GATE</span>
            <strong>{gateStatus(zone)}</strong>
          </div>
          <span class="flow-arrow" aria-hidden="true">→</span>
          <div class="flow-stage weather-stage" data-active={overlays.length > 0}>
            <span>WEATHER OVERLAY</span>
            {#if overlays.length > 0}
              {#each overlays as overlay (overlay.role)}
                <strong class="weather-output" data-phase={overlay.phase}>
                  {overlayTitle(overlay.effect)} · {overlay.role} ·
                  {overlay.phase === "none" ? "active" : overlay.phase}
                </strong>
              {/each}
            {:else}
              <strong>None active</strong>
            {/if}
          </div>
          <span class="flow-arrow" aria-hidden="true">→</span>
          <div class="flow-members">
            {#each zone.members as member (member.role)}
              {@const light = lightFor(member.role)}
              {@const isOn = light?.on ?? false}
              {@const weather = light && light.weatherEffect !== "none" ? light : null}
              {@const isPulsing = Boolean(pulse || weather)}
              {@const levelPercent = isPulsing ? pulseLevelPercent(member.role) : 0}
              <button
                class="flow-member"
                class:selected={selectedRole === member.role}
                data-pulse={pulse ?? undefined}
                data-weather={weather?.weatherEffect ?? undefined}
                type="button"
                aria-pressed={selectedRole === member.role}
                onclick={() => (selectedRole = member.role)}
              >
                <strong>{member.role}</strong>
                <span>{member.behavior}</span>
                <small
                  class="flow-member-state"
                  data-on={isOn}
                  data-phase={pulse ?? undefined}
                  data-weather={weather?.weatherEffect ?? undefined}
                >
                  {pulse
                    ? `PULSE ${pulse} · ${levelPercent}%`
                    : weather
                      ? `${overlayTitle(weather.weatherEffect).toUpperCase()} ${weather.pulsePhase.toUpperCase()} · ${levelPercent}%`
                      : isOn ? "ON" : "OFF"}
                </small>
                {#if isPulsing}
                  <i
                    class="flow-member-level"
                    data-weather={weather?.weatherEffect ?? undefined}
                    style={`--output-level: ${levelPercent}%`}
                    aria-hidden="true"
                  ></i>
                {/if}
              </button>
            {/each}
          </div>
        </div>
      </article>
    {/each}
  </div>

  {#if selectedMember}
    <section class="flow-curve" aria-label={`${selectedMember.role} daily target`}>
      <div class="flow-curve-heading">
        <div>
          <p class="eyebrow">24-HOUR TARGET · {zoneTitle(selectedZone?.zone ?? "")}</p>
          <h3>{selectedMember.role} · {selectedMember.behavior}</h3>
        </div>
        <span class="curve-live" data-on={lightIsOn(selectedMember.role)}>
          {lightIsOn(selectedMember.role) ? "LIVE ON" : "LIVE OFF"}
        </span>
      </div>
      <svg class="curve-chart" viewBox="0 0 640 166" role="img"
        aria-label={`${selectedMember.role} program target over 24 hours; current time ${snapshot.time}`}>
        <line class="curve-grid" x1="42" y1="30" x2="626" y2="30" />
        <line class="curve-grid" x1="42" y1="78" x2="626" y2="78" />
        <line class="curve-grid" x1="42" y1="126" x2="626" y2="126" />
        <text class="curve-level" x="3" y="33">100%</text>
        <text class="curve-level" x="13" y="81">50%</text>
        <text class="curve-level" x="24" y="129">0</text>
        {#each [0, 6, 12, 18, 24] as hour (hour)}
          {@const minute = Math.min(hour * 60, 1439)}
          <line class="curve-tick" x1={xFor(minute)} y1="126" x2={xFor(minute)} y2="131" />
          <text class="curve-hour" x={xFor(minute)} y="145" text-anchor={hour === 0 ? "start" : hour === 24 ? "end" : "middle"}>
            {String(hour).padStart(2, "0")}
          </text>
        {/each}
        <path class="curve-line" d={curvePath(selectedMember)} />
        <line class="curve-now" x1={xFor(snapshot.simMinute)} y1="21" x2={xFor(snapshot.simMinute)} y2="136" />
        {#if selectedMember.behavior !== "on/off"}
          {#each selectedMember.curve as sample, index (sample.minute)}
            {@const nextMinute = selectedMember.curve[index + 1]?.minute ?? 1440}
            <rect
              x={xFor(sample.minute)}
              y="151"
              width={(nextMinute - sample.minute) / 1439 * 584}
              height="7"
              fill={colorCss(sample.color)}
              opacity={sample.on ? 1 : 0}
            />
          {/each}
        {/if}
      </svg>
      <div class="flow-curve-meta">
        <span>{selectedMember.behavior === "on/off" ? "Scheduled output" : "Expected light output"}</span>
        <strong>{snapshot.time} now</strong>
      </div>
    </section>
  {/if}

  <p class="hint flow-note">For the office, double-press the Button for Red Alert, single-press to resume Daylight, or hold for Off. Red Alert bypasses the schedule gate; weather-alert overlays are separate. The chart remains the projected daylight target, not the active output in other modes.</p>
</section>
