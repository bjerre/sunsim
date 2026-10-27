<script lang="ts">
  import { onMount } from "svelte";
  import ControlsPane from "./ControlsPane.svelte";
  import DisplayPane from "./DisplayPane.svelte";
  import OfficePane from "./OfficePane.svelte";
  import ProgramFlowPane from "./ProgramFlowPane.svelte";
  import WindowPane from "./WindowPane.svelte";
  import {
    createSimulatorController,
    type AlertLevel,
    type ButtonPress,
    type LocationCoordinates,
    type Precipitation,
    type SimulationClock,
    type SimulatorController,
    type SimulatorState,
  } from "../simulator.ts";

  const savedLocationKey = "sunsim.location.v1";

  let simulator = $state<SimulatorController | null>(null);
  let simState = $state<SimulatorState | null>(null);
  let runtimeLabel = $state("Starting WebAssembly…");
  let eventStatus = $state("Waiting for first event…");
  let simulatedDate = $state("");
  let simulatedMinute = $state(720);
  let timeLabel = $state("Simulated time (UTC)");
  let timeZoneDescription = $state("UTC · default Greenwich coordinates");
  let locationInfo = $state("The browser will ask to use your location on load. If allowed, coordinates are saved only in this browser.");
  let locationPending = $state(false);
  let usesBrowserTimezone = false;
  let hasCoordinates = false;
  let clockTimestamp = Date.now();

  function isLocationCoordinates(value: unknown): value is LocationCoordinates {
    if (!value || typeof value !== "object") return false;
    const coordinates = value as Partial<LocationCoordinates>;
    return typeof coordinates.latitude === "number" && Number.isFinite(coordinates.latitude) &&
      coordinates.latitude >= -90 && coordinates.latitude <= 90 &&
      typeof coordinates.longitude === "number" && Number.isFinite(coordinates.longitude) &&
      coordinates.longitude >= -180 && coordinates.longitude <= 180;
  }

  function readSavedLocation(): LocationCoordinates | null {
    try {
      const value: unknown = JSON.parse(localStorage.getItem(savedLocationKey) ?? "null");
      return isLocationCoordinates(value) ? value : null;
    } catch {
      return null;
    }
  }

  function saveLocation(coordinates: LocationCoordinates): boolean {
    try {
      localStorage.setItem(savedLocationKey, JSON.stringify(coordinates));
      return true;
    } catch {
      return false;
    }
  }

  function readSimulationClock(date: Date): SimulationClock {
    return {
      year: usesBrowserTimezone ? date.getFullYear() : date.getUTCFullYear(),
      month: (usesBrowserTimezone ? date.getMonth() : date.getUTCMonth()) + 1,
      day: usesBrowserTimezone ? date.getDate() : date.getUTCDate(),
      hour: usesBrowserTimezone ? date.getHours() : date.getUTCHours(),
      minute: usesBrowserTimezone ? date.getMinutes() : date.getUTCMinutes(),
      utcOffsetMinutes: usesBrowserTimezone ? -date.getTimezoneOffset() : 0,
    };
  }

  function updateTimeZoneInfo(date: Date): void {
    if (!usesBrowserTimezone) {
      timeLabel = "Simulated time (UTC)";
      timeZoneDescription = "UTC · default Greenwich coordinates";
      return;
    }
    const offset = -date.getTimezoneOffset();
    const sign = offset < 0 ? "−" : "+";
    const hours = String(Math.floor(Math.abs(offset) / 60)).padStart(2, "0");
    const minutes = String(Math.abs(offset) % 60).padStart(2, "0");
    const zoneName = Intl.DateTimeFormat().resolvedOptions().timeZone || "browser timezone";
    timeLabel = "Simulated local time";
    timeZoneDescription = `${zoneName} · UTC${sign}${hours}:${minutes}`;
  }

  function applyClock(date: Date): void {
    if (!simulator) return;
    clockTimestamp = date.getTime();
    const clock = readSimulationClock(date);
    simulatedDate = `${clock.year}-${String(clock.month).padStart(2, "0")}-${String(clock.day).padStart(2, "0")}`;
    simulatedMinute = clock.hour * 60 + clock.minute;
    updateTimeZoneInfo(date);
    if (!simulator.setClock(clock)) {
      throw new Error("Could not set the simulated date and time.");
    }
  }

  function setClockFromControls(dateValue: string, minuteOfDay: number): void {
    if (!dateValue) return;
    const [year, month, day] = dateValue.split("-").map(Number);
    const hour = Math.floor(minuteOfDay / 60);
    const minute = minuteOfDay % 60;
    const date = usesBrowserTimezone
      ? new Date(year, month - 1, day, hour, minute)
      : new Date(Date.UTC(year, month - 1, day, hour, minute));
    applyClock(date);
  }

  function requestLocation(): void {
    if (!navigator.geolocation) {
      locationInfo = hasCoordinates
        ? "Browser location is unavailable; continuing with the current coordinates."
        : "Browser location is unavailable; continuing with the Greenwich/UTC demo.";
      return;
    }
    locationPending = true;
    locationInfo = "Requesting browser location permission…";
    navigator.geolocation.getCurrentPosition(
      ({ coords }) => {
        locationPending = false;
        const coordinates = { latitude: coords.latitude, longitude: coords.longitude };
        if (!simulator?.setLocation(coordinates.latitude, coordinates.longitude)) {
          locationInfo = "The browser returned coordinates outside the valid range.";
          return;
        }
        usesBrowserTimezone = true;
        hasCoordinates = true;
        const saved = saveLocation(coordinates);
        locationInfo = saved
          ? `Using browser coordinates (±${Math.round(coords.accuracy)} m). Saved in this browser only.`
          : `Using browser coordinates (±${Math.round(coords.accuracy)} m). Could not save them in this browser.`;
        eventStatus = "Location updated the daylight calculation";
        applyClock(new Date());
      },
      (error) => {
        locationPending = false;
        const messages: Record<number, string> = {
          1: "Location permission was denied.",
          2: "The browser could not determine a location.",
          3: "Location request timed out.",
        };
        const reason = messages[error.code] ?? "Could not read browser location.";
        locationInfo = hasCoordinates
          ? `${reason} Continuing with the current coordinates.`
          : `${reason} Continuing with the Greenwich/UTC demo.`;
      },
      { enableHighAccuracy: false, timeout: 15000, maximumAge: 300000 },
    );
  }

  function pressButton(kind: ButtonPress): void {
    simulator?.pressButton(kind);
    eventStatus = `Button ${["single", "double", "hold"][kind]} → core event`;
  }

  function updateWeather(precipitation: Precipitation, cloudPercent: number): void {
    simulator?.setWeather(precipitation, cloudPercent);
    eventStatus = "Weather conditions posted to event bus";
  }

  function updateAlert(level: AlertLevel): void {
    simulator?.setAlert(level);
    eventStatus = "Weather alert posted to event bus";
  }

  onMount(() => {
    let alive = true;
    let unsubscribe: (() => void) | undefined;
    let clockTimer: number | undefined;
    let activeSimulator: SimulatorController | undefined;

    async function start(): Promise<void> {
      try {
        activeSimulator = await createSimulatorController();
        if (!alive) {
          activeSimulator.dispose();
          return;
        }
        simulator = activeSimulator;

        const savedLocation = readSavedLocation();
        hasCoordinates = savedLocation !== null &&
          activeSimulator.setLocation(savedLocation.latitude, savedLocation.longitude);
        usesBrowserTimezone = hasCoordinates;
        unsubscribe = activeSimulator.subscribe((nextState) => {
          simState = nextState;
          simulatedDate = nextState.simDate;
          simulatedMinute = nextState.simMinute;
        });
        runtimeLabel = "WebAssembly core online";
        const initialNow = new Date();
        applyClock(initialNow);
        eventStatus = "Tick and program events running";
        clockTimer = window.setInterval(() => {
          const nextTime = new Date(clockTimestamp + 60_000);
          activeSimulator?.advance(1);
          applyClock(nextTime);
        }, 1000);
        requestLocation();
      } catch (error: unknown) {
        if (!alive) return;
        runtimeLabel = "WebAssembly failed to start";
        eventStatus = error instanceof Error ? error.message : String(error);
        console.error(error);
      }
    }

    void start();
    return () => {
      alive = false;
      if (clockTimer !== undefined) window.clearInterval(clockTimer);
      unsubscribe?.();
      activeSimulator?.dispose();
    };
  });
</script>

<main class="shell">
  <header class="topbar">
    <div>
      <p class="eyebrow">SUNSIM · PORTABLE CORE</p>
      <h1>Office lighting lab</h1>
    </div>
    <div class="runtime"><span class="live-dot"></span><span>{runtimeLabel}</span></div>
  </header>

  {#if simState}
    <section class="workspace">
      <div class="main-column">
        <OfficePane snapshot={simState} />
        <ProgramFlowPane snapshot={simState} onButtonPress={pressButton} />
      </div>
      <aside class="side-column">
        <WindowPane snapshot={simState} />
        <DisplayPane snapshot={simState} />
        <ControlsPane
          snapshot={simState}
          date={simulatedDate}
          minute={simulatedMinute}
          {timeLabel}
          {timeZoneDescription}
          {locationInfo}
          {locationPending}
          onClockChange={setClockFromControls}
          onRequestLocation={requestLocation}
          onButtonPress={pressButton}
          onWeatherChange={updateWeather}
          onAlertChange={updateAlert}
        />
      </aside>
    </section>
  {:else}
    <section class="panel loading" aria-live="polite">
      <p class="eyebrow">SIMULATION CORE</p>
      <h2>{runtimeLabel}</h2>
      <p class="hint">{eventStatus}</p>
    </section>
  {/if}
  <footer><span>Core logic: C++ → WebAssembly</span><span>{eventStatus}</span></footer>
</main>
