<script lang="ts">
  import type {
    AlertLevel,
    ButtonPress,
    Precipitation,
    SimulatorState,
  } from "../simulator.js";

  interface Props {
    snapshot: SimulatorState;
    date: string;
    minute: number;
    timeLabel: string;
    timeZoneDescription: string;
    locationInfo: string;
    locationPending: boolean;
    onClockChange: (date: string, minute: number) => void;
    onRequestLocation: () => void;
    onButtonPress: (kind: ButtonPress) => void;
    onWeatherChange: (precipitation: Precipitation, cloudPercent: number) => void;
    onAlertChange: (level: AlertLevel) => void;
  }

  let {
    snapshot,
    date,
    minute,
    timeLabel,
    timeZoneDescription,
    locationInfo,
    locationPending,
    onClockChange,
    onRequestLocation,
    onButtonPress,
    onWeatherChange,
    onAlertChange,
  }: Props = $props();

  let precipitation = $state<Precipitation>(0);
  let cloudPercent = $state(0);
  let alertLevel = $state<AlertLevel>(0);

  function updateWeather(): void {
    onWeatherChange(precipitation, cloudPercent);
  }
</script>

<section class="controls panel" aria-label="Simulation controls">
  <div class="section-heading compact">
    <div><p class="eyebrow">EVENT INPUTS</p><h2>Virtual controls</h2></div>
  </div>
  <div class="control-group">
    <label for="date-picker"><span>Simulated date</span></label>
    <input
      id="date-picker"
      type="date"
      value={date}
      onchange={(event) => onClockChange(event.currentTarget.value, minute)}
    />
    <p class="hint">Choose a day, then scrub time to replay the office schedule.</p>
  </div>
  <div class="control-group">
    <label for="time-slider"><span>{timeLabel}</span><output>{String(Math.floor(minute / 60)).padStart(2, "0")}:{String(minute % 60).padStart(2, "0")}</output></label>
    <input
      id="time-slider"
      type="range"
      min="0"
      max="1439"
      step="1"
      value={minute}
      oninput={(event) => onClockChange(date, Number(event.currentTarget.value))}
    />
    <div class="range-labels"><span>00:00</span><span>23:59</span></div>
    <p class="hint">{timeZoneDescription}</p>
  </div>
  <div class="control-group">
    <span class="control-title">Sun location <output>{snapshot.location.latitude.toFixed(2)}°, {snapshot.location.longitude.toFixed(2)}°</output></span>
    <button class="location-button" type="button" disabled={locationPending} onclick={onRequestLocation}>
      {locationPending ? "Finding location…" : "Use / refresh my location"}
    </button>
    <p class="hint">{locationInfo}</p>
  </div>
  <div class="control-group">
    <span class="control-title">Wall button</span>
    <div class="button-row">
      <button class="button-control" type="button" onclick={() => onButtonPress(0)}><span>●</span>Single</button>
      <button class="button-control" type="button" onclick={() => onButtonPress(1)}><span>●●</span>Double</button>
      <button class="button-control" type="button" onclick={() => onButtonPress(2)}><span>◉</span>Hold</button>
    </div>
    <p class="hint">Uses the same button event and mode controller as the device.</p>
  </div>
  <div class="control-group">
    <label for="weather-select"><span>Weather sensor</span></label>
    <select
      id="weather-select"
      value={precipitation}
      onchange={(event) => {
        precipitation = Number(event.currentTarget.value) as Precipitation;
        updateWeather();
      }}
    >
      <option value={0}>Clear</option>
      <option value={1}>Drizzle</option>
      <option value={2}>Rain</option>
      <option value={3}>Freezing rain</option>
      <option value={4}>Snow</option>
      <option value={5}>Thunderstorm</option>
    </select>
    <label class="sub-label" for="cloud-slider"><span>Cloud cover</span><output>{cloudPercent}%</output></label>
    <input
      id="cloud-slider"
      type="range"
      min="0"
      max="100"
      value={cloudPercent}
      oninput={(event) => {
        cloudPercent = Number(event.currentTarget.value);
        updateWeather();
      }}
    />
    <p class="hint">Cloud cover sets the simulated forecast for the whole day and dims the wall-light curve.</p>
  </div>
  <div class="control-group last">
    <label for="alert-select"><span>Weather alert</span></label>
    <select
      id="alert-select"
      value={alertLevel}
      onchange={(event) => {
        alertLevel = Number(event.currentTarget.value) as AlertLevel;
        onAlertChange(alertLevel);
      }}
    >
      <option value={0}>None</option>
      <option value={1}>Advisory</option>
      <option value={2}>Watch</option>
      <option value={3}>Warning</option>
    </select>
    <p class="hint">Inputs post ConditionsUpdated and AlertsUpdated through the shared event bus.</p>
  </div>
</section>
