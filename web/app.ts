const wallRoles = ["WallA", "WallB", "WallC", "WallD", "WallE"] as const;
type WallRole = (typeof wallRoles)[number];
type LightRole = WallRole | "Work";
type Rgb = [number, number, number];
type LightColor =
  | { mode: "mireds"; value: number }
  | { mode: "xy"; x: number; y: number }
  | { mode: "hue"; hue: number; saturation: number };

interface SimLight {
  role: LightRole;
  on: boolean;
  level: number;
  color: LightColor;
}

interface SimulatorState {
  location: { latitude: number; longitude: number };
  utcOffsetMinutes: number;
  time: string;
  weekday: string;
  simDate: string;
  sun: { azimuthDeg: number; elevationDeg: number };
  dayLength: string;
  nextEvent: string;
  mode: string;
  modeTone: string;
  sky: string;
  skyTone: string;
  simMinute: number;
  lights: SimLight[];
  devices: { role: LightRole | "Button"; link: string; endpoint: boolean }[];
}

interface SimulationClock {
  year: number;
  month: number;
  day: number;
  hour: number;
  minute: number;
  utcOffsetMinutes: number;
}

interface LocationCoordinates {
  latitude: number;
  longitude: number;
}

const savedLocationKey = "sunsim.location.v1";

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

function element<T extends Element>(selector: string): T {
  const found = document.querySelector<T>(selector);
  if (!found) throw new Error(`Missing browser element: ${selector}`);
  return found;
}

const runtimeLabel = element<HTMLElement>("#runtime-label");
const statusLabel = element<HTMLElement>("#event-status");
const wallLights = element<HTMLDivElement>("#wall-lights");

function makeLight(role: WallRole): HTMLDivElement {
  const item = document.createElement("div");
  item.className = "bulb-unit";
  const uplight = document.createElement("span");
  uplight.className = "wall-uplight";
  const bulb = document.createElement("span");
  bulb.className = "bulb";
  const name = document.createElement("span");
  name.textContent = role;
  const state = document.createElement("small");
  state.textContent = "OFF";
  item.append(uplight, bulb, name, state);
  return item;
}

const bulbElements = new Map<WallRole, HTMLDivElement>();
for (const role of wallRoles) {
  const light = makeLight(role);
  wallLights.append(light);
  bulbElements.set(role, light);
}

async function start(): Promise<void> {
  try {
    const { default: createSunsim } = await import("../sunsim_build2_smoke.js");
    const core = await createSunsim();
    core._sunsim_init();
    runtimeLabel.textContent = "WebAssembly core online";

    const display = {
      time: element<HTMLElement>("#lcd-time"),
      weekday: element<HTMLElement>("#lcd-weekday"),
      mode: element<HTMLElement>("#lcd-mode"),
      day: element<HTMLElement>("#lcd-day"),
      next: element<HTMLElement>("#lcd-next"),
      sky: element<HTMLElement>("#lcd-sky"),
      lights: element<HTMLElement>("#lcd-lights"),
    };
    const locationValue = element<HTMLOutputElement>("#location-value");
    const locationInfo = element<HTMLElement>("#location-info");
    const locationButton = element<HTMLButtonElement>("#location-button");
    const timeZoneInfo = element<HTMLElement>("#time-zone-info");
    const datePicker = element<HTMLInputElement>("#date-picker");
    const timeSlider = element<HTMLInputElement>("#time-slider");
    const timeLabel = element<HTMLElement>("#time-label");
    const windowView = element<HTMLDivElement>("#window-view");
    const sunMarker = element<HTMLSpanElement>("#window-sun");
    const sunPositionLabel = element<HTMLElement>("#sun-position-label");
    const torchiereStatus = element<HTMLElement>("#torchiere-status");
    const weatherSelect = element<HTMLSelectElement>("#weather-select");
    const cloudSlider = element<HTMLInputElement>("#cloud-slider");
    const cloudValue = element<HTMLOutputElement>("#cloud-value");
    const alertSelect = element<HTMLSelectElement>("#alert-select");
    let usesBrowserTimezone = false;
    let clockTimestamp = 0;
    const savedLocation = readSavedLocation();
    let hasSavedLocation = savedLocation !== null &&
      core._sunsim_set_location(savedLocation.latitude, savedLocation.longitude) !== 0;
    usesBrowserTimezone = hasSavedLocation;

    function readSimulationClock(date: Date): SimulationClock {
      // The default Greenwich demo uses UTC. After location permission, use
      // the browser timezone alongside the browser's coordinates.
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
        timeLabel.textContent = "Simulated time (UTC)";
        timeZoneInfo.textContent = "UTC · default Greenwich coordinates";
        return;
      }
      const offset = -date.getTimezoneOffset();
      const sign = offset < 0 ? "−" : "+";
      const hours = String(Math.floor(Math.abs(offset) / 60)).padStart(2, "0");
      const minutes = String(Math.abs(offset) % 60).padStart(2, "0");
      const zoneName = Intl.DateTimeFormat().resolvedOptions().timeZone || "browser timezone";
      timeLabel.textContent = "Simulated local time";
      timeZoneInfo.textContent = `${zoneName} · UTC${sign}${hours}:${minutes}`;
    }

    function applyClock(date: Date): void {
      clockTimestamp = date.getTime();
      const simulationClock = readSimulationClock(date);
      updateTimeZoneInfo(date);
      if (!core._sunsim_set_clock(
        simulationClock.year,
        simulationClock.month,
        simulationClock.day,
        simulationClock.hour,
        simulationClock.minute,
        simulationClock.utcOffsetMinutes,
      )) {
        throw new Error("Could not set the simulated date and time.");
      }
      sync();
    }

    function clockFromControls(): void {
      if (!datePicker.value) return;
      const [year, month, day] = datePicker.value.split("-").map(Number);
      const minuteOfDay = Number(timeSlider.value);
      const hour = Math.floor(minuteOfDay / 60);
      const minute = minuteOfDay % 60;
      const date = usesBrowserTimezone
        ? new Date(year, month - 1, day, hour, minute)
        : new Date(Date.UTC(year, month - 1, day, hour, minute));
      applyClock(date);
    }

    function requestLocation(): void {
      if (!navigator.geolocation) {
        locationInfo.textContent = hasSavedLocation
          ? "Browser location is unavailable; continuing with saved coordinates."
          : "Browser location is unavailable; continuing with the Greenwich/UTC demo.";
        return;
      }
      locationButton.disabled = true;
      locationInfo.textContent = "Requesting browser location permission…";
      navigator.geolocation.getCurrentPosition(
        ({ coords }) => {
          locationButton.disabled = false;
          const coordinates = { latitude: coords.latitude, longitude: coords.longitude };
          if (!core._sunsim_set_location(coordinates.latitude, coordinates.longitude)) {
            locationInfo.textContent = "The browser returned coordinates outside the valid range.";
            return;
          }
          usesBrowserTimezone = true;
          hasSavedLocation = saveLocation(coordinates);
          locationInfo.textContent = hasSavedLocation
            ? `Using browser coordinates (±${Math.round(coords.accuracy)} m). Saved in this browser only.`
            : `Using browser coordinates (±${Math.round(coords.accuracy)} m). Could not save them in this browser.`;
          statusLabel.textContent = "Location updated the daylight calculation";
          applyClock(new Date());
        },
        (error) => {
          locationButton.disabled = false;
          const messages: Record<number, string> = {
            1: "Location permission was denied.",
            2: "The browser could not determine a location.",
            3: "Location request timed out.",
          };
          const reason = messages[error.code] ?? "Could not read browser location.";
          locationInfo.textContent = hasSavedLocation
            ? `${reason} Continuing with saved coordinates.`
            : `${reason} Continuing with the Greenwich/UTC demo.`;
        },
        { enableHighAccuracy: false, timeout: 15000, maximumAge: 300000 },
      );
    }

    function temperatureRgb(mireds: number): Rgb {
      const kelvin = Math.max(1800, Math.min(6500, 1_000_000 / Math.max(1, mireds)));
      const t = (kelvin - 1800) / (6500 - 1800);
      const r = Math.round(255 - 75 * t);
      const g = Math.round(183 + 53 * t);
      const b = Math.round(105 + 126 * t);
      return [r, g, b];
    }

    function colorRgb(color: LightColor): Rgb {
      if (color.mode === "mireds") return temperatureRgb(color.value);
      if (color.mode === "xy") {
        const y = Math.max(0.0001, color.y);
        const x = color.x / y;
        const z = (1 - color.x - color.y) / y;
        const linear = [
          3.2406 * x - 1.5372 - 0.4986 * z,
          -0.9689 * x + 1.8758 + 0.0415 * z,
          0.0557 * x - 0.204 + 1.057 * z,
        ];
        const scale = Math.max(...linear, 0.0001);
        return linear.map((channel) => {
          const value = Math.max(0, Math.min(1, channel / scale));
          const srgb = value <= 0.0031308 ? 12.92 * value : 1.055 * value ** (1 / 2.4) - 0.055;
          return Math.round(srgb * 255);
        }) as Rgb;
      }
      const hue = ((color.hue * 360 / 254) % 360 + 360) % 360;
      const saturation = color.saturation / 254;
      const lightness = 0.58;
      const chroma = (1 - Math.abs(2 * lightness - 1)) * saturation;
      const x = chroma * (1 - Math.abs((hue / 60) % 2 - 1));
      const [r, g, b] = hue < 60 ? [chroma, x, 0]
        : hue < 120 ? [x, chroma, 0]
          : hue < 180 ? [0, chroma, x]
            : hue < 240 ? [0, x, chroma]
              : hue < 300 ? [x, 0, chroma]
                : [chroma, 0, x];
      const m = lightness - chroma / 2;
      return [r, g, b].map((channel) => Math.round((channel + m) * 255)) as Rgb;
    }

    function normalizedIntensity(level: number): number {
      return Math.max(0, Math.min(1, level / 254));
    }

    function lightGlowOpacity(level: number): string {
      const intensity = Math.max(0.16, level / 254);
      return String(0.2 + intensity * 0.65);
    }

    function temperatureColor(mireds: number): string {
      return `rgb(${temperatureRgb(mireds).join(" ")})`;
    }

    function drawWindow(state: SimulatorState): void {
      const relativeAzimuth = ((state.sun.azimuthDeg - 180 + 540) % 360) - 180;
      const elevation = Math.max(0, Math.min(90, state.sun.elevationDeg));
      const inSouthFacingView = state.sun.elevationDeg > 0 && Math.abs(relativeAzimuth) <= 90;
      sunMarker.style.left = `${50 + relativeAzimuth / 180 * 100}%`;
      sunMarker.style.top = `${100 - elevation / 90 * 100}%`;
      windowView.classList.toggle("sun-visible", inSouthFacingView);
      windowView.style.setProperty("--window-brightness", String(0.15 + 0.85 * Math.sin(elevation * Math.PI / 180)));
      const direction = relativeAzimuth < -15 ? "east of south"
        : relativeAzimuth > 15 ? "west of south" : "south";
      sunPositionLabel.textContent = state.sun.elevationDeg > 0
        ? `${Math.round(state.sun.elevationDeg)}° · ${direction}`
        : "Below horizon";

      const daylightColors = state.lights
        .filter((light) => light.role.startsWith("Wall") && light.color.mode !== "hue")
        .map((light) => colorRgb(light.color));
      const total = daylightColors.reduce<Rgb>(
        (sum, rgb) => [sum[0] + rgb[0], sum[1] + rgb[1], sum[2] + rgb[2]],
        [0, 0, 0],
      );
      const tint: Rgb = daylightColors.length
        ? total.map((channel) => Math.round(channel / daylightColors.length)) as Rgb
        : [143, 174, 170];
      windowView.style.setProperty("--window-tint", `rgb(${tint.join(" ")})`);
    }

    function draw(state: SimulatorState): void {
      drawWindow(state);
      const onLights = state.lights.filter((light) => light.on).length;
      const modeBadge = element<HTMLElement>("#mode-badge");
      modeBadge.textContent = state.mode || "Idle";
      modeBadge.dataset.tone = state.modeTone;
      display.time.textContent = state.time;
      display.weekday.textContent = state.weekday || "SIMULATED";
      display.mode.textContent = state.mode || "Waiting";
      display.day.textContent = state.dayLength;
      display.next.textContent = state.nextEvent;
      display.sky.textContent = state.sky || "clear";
      display.lights.textContent = `${onLights} / 6`;
      element<HTMLElement>("#sky-label").textContent = state.sky ? `Sky: ${state.sky}` : "Clear sky";
      element<HTMLElement>("#device-count").textContent =
        `${state.devices.filter((device) => device.link === "online").length} / ${state.devices.length} online`;
      locationValue.textContent =
        `${state.location.latitude.toFixed(2)}°, ${state.location.longitude.toFixed(2)}°`;
      datePicker.value = state.simDate;
      timeSlider.value = String(state.simMinute);
      element<HTMLOutputElement>("#time-value").textContent =
        `${String(Math.floor(state.simMinute / 60)).padStart(2, "0")}:${String(state.simMinute % 60).padStart(2, "0")}`;

      for (const light of state.lights) {
        if (light.role === "Work") {
          const color = colorRgb(light.color);
          const work = element<HTMLDivElement>("#work-light");
          const glow = element<HTMLSpanElement>("#work-light .torchiere-glow");
          const shade = element<HTMLSpanElement>("#work-light .torchiere-shade");
          glow.style.opacity = light.on ? lightGlowOpacity(light.level) : "0";
          glow.style.setProperty("--torchiere-color", `rgb(${color.join(" ")})`);
          shade.style.background = light.on ? `rgb(${color.join(" ")})` : "";
          work.style.filter = light.on ? `drop-shadow(0 0 10px rgb(${color.join(" ")} / 55%))` : "";
          torchiereStatus.textContent = light.on
            ? `ON · ${Math.round(light.level / 254 * 100)}% · ${Math.round(1_000_000 / (light.color.mode === "mireds" ? light.color.value : 250))} K`
            : state.mode === "off" ? "OFF · manual override" : "OFF · outside work schedule";
          continue;
        }
        const lightElement = bulbElements.get(light.role);
        if (!lightElement) continue;
        const uplight = lightElement.querySelector<HTMLSpanElement>(".wall-uplight");
        const bulb = lightElement.querySelector<HTMLSpanElement>(".bulb");
        const label = lightElement.querySelector<HTMLElement>("small");
        if (!uplight || !bulb || !label) continue;
        const color = light.color.mode === "mireds" ? temperatureColor(light.color.value)
          : light.color.mode === "hue" ? `hsl(${Math.round(light.color.hue * 360 / 254)} ${Math.round(light.color.saturation * 100 / 254)}% 58%)`
          : `rgb(${colorRgb(light.color).join(" ")})`;
        const intensity = normalizedIntensity(light.level);
        uplight.style.setProperty("--wall-uplight-color", color);
        uplight.style.opacity = light.on ? String(0.15 + intensity * 0.8) : "0";
        bulb.style.background = light.on ? `radial-gradient(circle at 35% 30%, #fff8, ${color} 48%, #514738)` : "#29332b";
        bulb.style.filter = light.on ? `brightness(${0.15 + intensity * 0.85})` : "";
        bulb.style.boxShadow = light.on ? `0 0 ${8 + intensity * 24}px ${color}aa, 0 0 ${20 + intensity * 32}px ${color}55` : "none";
        label.textContent = light.on ? `${Math.round(light.level / 254 * 100)}%` : "OFF";
        label.style.color = light.on ? color : "";
      }
    }

    function sync(): void {
      draw(JSON.parse(core.UTF8ToString(core._sunsim_state())) as SimulatorState);
    }

    const initialNow = new Date();
    const initialClock = readSimulationClock(initialNow);
    datePicker.value = `${initialClock.year}-${String(initialClock.month).padStart(2, "0")}-${String(initialClock.day).padStart(2, "0")}`;
    timeSlider.value = String(initialClock.hour * 60 + initialClock.minute);
    applyClock(initialNow);
    setInterval(() => {
      const nextTime = new Date(clockTimestamp + 60_000);
      core._sunsim_advance(1);
      applyClock(nextTime);
    }, 1000);

    datePicker.addEventListener("change", clockFromControls);
    timeSlider.addEventListener("input", clockFromControls);
    locationButton.addEventListener("click", requestLocation);
    document.querySelectorAll<HTMLButtonElement>("[data-press]").forEach((button) => {
      button.addEventListener("click", () => {
        core._sunsim_press(Number(button.dataset.press));
        statusLabel.textContent = `Button ${button.textContent.trim().toLowerCase()} → core event`;
        sync();
      });
    });
    const updateWeather = () => {
      core._sunsim_weather(Number(weatherSelect.value), Number(cloudSlider.value));
      cloudValue.textContent = `${cloudSlider.value}%`;
      statusLabel.textContent = "Weather conditions posted to event bus";
      sync();
    };
    weatherSelect.addEventListener("change", updateWeather);
    cloudSlider.addEventListener("input", updateWeather);
    alertSelect.addEventListener("change", () => {
      core._sunsim_alert(Number(alertSelect.value));
      statusLabel.textContent = "Weather alert posted to event bus";
      sync();
    });
    statusLabel.textContent = "Tick and program events running";
    requestLocation();
  } catch (error: unknown) {
    runtimeLabel.textContent = "WebAssembly failed to start";
    statusLabel.textContent = error instanceof Error ? error.message : String(error);
    console.error(error);
  }
}

start();
