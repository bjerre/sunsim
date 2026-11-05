export type WallRole = "WallA" | "WallB" | "WallC" | "WallD" | "WallE";
export type LightRole = WallRole | "Work" | "Desk" | "Grow";
export type DeviceRole = LightRole | "Button";

export type LightColor =
  | { mode: "mireds"; value: number }
  | { mode: "xy"; x: number; y: number }
  | { mode: "hue"; hue: number; saturation: number };

export interface SimLight {
  role: LightRole;
  on: boolean;
  level: number;
  color: LightColor;
}

export interface SimulatorState {
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
  devices: { role: DeviceRole; link: string; endpoint: boolean }[];
}

export interface SimulationClock {
  year: number;
  month: number;
  day: number;
  hour: number;
  minute: number;
  utcOffsetMinutes: number;
}

export interface LocationCoordinates {
  latitude: number;
  longitude: number;
}

export type ButtonPress = 0 | 1 | 2;
export type Precipitation = 0 | 1 | 2 | 3 | 4 | 5;
export type AlertLevel = 0 | 1 | 2 | 3;

export interface SimulatorController {
  readonly snapshot: SimulatorState;
  subscribe(listener: (state: SimulatorState) => void): () => void;
  setClock(clock: SimulationClock): boolean;
  setLocation(latitude: number, longitude: number): boolean;
  advance(seconds: number): void;
  pressButton(kind: ButtonPress): void;
  setWeather(precipitation: Precipitation, cloudPercent: number): void;
  setAlert(level: AlertLevel): void;
  dispose(): void;
}

class WasmSimulatorController implements SimulatorController {
  private current: SimulatorState;
  private readonly listeners = new Set<(state: SimulatorState) => void>();

  constructor(private readonly module: SunsimModule) {
    module._sunsim_init();
    this.current = this.readState();
  }

  get snapshot(): SimulatorState {
    return this.current;
  }

  subscribe(listener: (state: SimulatorState) => void): () => void {
    this.listeners.add(listener);
    listener(this.current);
    return () => this.listeners.delete(listener);
  }

  setClock(clock: SimulationClock): boolean {
    const accepted = this.module._sunsim_set_clock(
      clock.year,
      clock.month,
      clock.day,
      clock.hour,
      clock.minute,
      clock.utcOffsetMinutes,
    ) !== 0;
    if (accepted) this.publish();
    return accepted;
  }

  setLocation(latitude: number, longitude: number): boolean {
    const accepted = this.module._sunsim_set_location(latitude, longitude) !== 0;
    if (accepted) this.publish();
    return accepted;
  }

  advance(seconds: number): void {
    this.module._sunsim_advance(seconds);
    this.publish();
  }

  pressButton(kind: ButtonPress): void {
    this.module._sunsim_press(kind);
    this.publish();
  }

  setWeather(precipitation: Precipitation, cloudPercent: number): void {
    this.module._sunsim_weather(precipitation, cloudPercent);
    this.publish();
  }

  setAlert(level: AlertLevel): void {
    this.module._sunsim_alert(level);
    this.publish();
  }

  dispose(): void {
    this.listeners.clear();
  }

  private readState(): SimulatorState {
    return JSON.parse(this.module.UTF8ToString(this.module._sunsim_state())) as SimulatorState;
  }

  private publish(): void {
    this.current = this.readState();
    for (const listener of this.listeners) listener(this.current);
  }
}

export async function createSimulatorController(): Promise<SimulatorController> {
  const { default: createSunsim } = await import("../sunsim_web_simulator.js");
  return new WasmSimulatorController(await createSunsim());
}
