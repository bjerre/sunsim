export type WallRole = "WallA" | "WallB" | "WallC" | "WallD" | "WallE";
export type LightRole = WallRole | "Work" | "Desk" | "Grow";
export type DeviceRole = LightRole | "Button";
export type WeatherEffect =
  | "none"
  | "drizzle"
  | "rain"
  | "freezing rain"
  | "snow"
  | "thunderstorm"
  | "advisory"
  | "watch"
  | "warning";
export type PulsePhase = "none" | "bright" | "dim";
export type RuntimeLook = "sun-curve" | "work-curve" | "on-off";

export interface RuntimeProfileMember {
  role: LightRole;
  look: RuntimeLook;
  peakPercent?: number;
}

export interface RuntimeProfileWeather {
  precipitation?: LightRole[];
  alerts?: LightRole[];
}

export interface RuntimeProfileZone {
  name: string;
  startMinute: number;
  endMinute: number;
  buttonRole?: "Button";
  members: RuntimeProfileMember[];
  weather?: RuntimeProfileWeather;
}

// Runtime profiles can regroup and reschedule the supported device roles.
// Device kinds and the role catalogue remain a build-time capability boundary.
export interface RuntimeInstallationProfile {
  schemaVersion: 1;
  zones: RuntimeProfileZone[];
}

export type LightColor =
  | { mode: "mireds"; value: number }
  | { mode: "xy"; x: number; y: number }
  | { mode: "hue"; hue: number; saturation: number };

export interface SimLight {
  role: LightRole;
  on: boolean;
  level: number;
  weatherEffect: WeatherEffect;
  pulsePhase: PulsePhase;
  color: LightColor;
}

export interface ProgramFlowMember {
  role: LightRole;
  behavior: string;
  curve: ProgramCurveSample[];
}

export interface ProgramCurveSample {
  minute: number;
  on: boolean;
  level: number;
  color: LightColor;
}

export interface ProgramFlowZone {
  zone: string;
  mode: string;
  gateOpen: boolean;
  gateApplies: boolean;
  availableModes: ProgramFlowMode[];
  members: ProgramFlowMember[];
}

export interface ProgramFlowMode {
  mode: string;
  buttonPress: "single" | "double" | "hold" | null;
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
  sunrise: string;
  sunset: string;
  cloudPercent: number | null;
  precipitation: string;
  mode: string;
  modeTone: string;
  sky: string;
  skyTone: string;
  simMinute: number;
  programFlow: ProgramFlowZone[];
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

const roleIndices: Record<DeviceRole, number> = {
  WallA: 0,
  WallB: 1,
  WallC: 2,
  WallD: 3,
  WallE: 4,
  Work: 5,
  Desk: 6,
  Grow: 7,
  Button: 8,
};
const lookIndices: Record<RuntimeLook, number> = {
  "sun-curve": 0,
  "work-curve": 1,
  "on-off": 2,
};

export function parseRuntimeInstallationProfile(value: unknown): RuntimeInstallationProfile {
  if (!value || typeof value !== "object") throw new Error("Installation profile must be a JSON object.");
  const document = value as Record<string, unknown>;
  if (document.schemaVersion !== 1 || !Array.isArray(document.zones) ||
      document.zones.length < 1 || document.zones.length > 8) {
    throw new Error("Installation profile must be schema version 1 and contain 1–8 zones.");
  }
  const knownRoles = new Set<LightRole>([
    "WallA", "WallB", "WallC", "WallD", "WallE", "Work", "Desk", "Grow",
  ]);
  const zones: RuntimeProfileZone[] = document.zones.map((item, index) => {
    if (!item || typeof item !== "object") throw new Error(`Zone ${index} must be an object.`);
    const zone = item as Record<string, unknown>;
    if (typeof zone.name !== "string" || typeof zone.startMinute !== "number" ||
        typeof zone.endMinute !== "number" || !Array.isArray(zone.members)) {
      throw new Error(`Zone ${index} needs a name, time window, and members array.`);
    }
    const members: RuntimeProfileMember[] = zone.members.map((memberValue, memberIndex) => {
      if (!memberValue || typeof memberValue !== "object") {
        throw new Error(`Zone ${zone.name} member ${memberIndex} must be an object.`);
      }
      const member = memberValue as Record<string, unknown>;
      if (typeof member.role !== "string" || !knownRoles.has(member.role as LightRole) ||
          typeof member.look !== "string" || !(member.look in lookIndices)) {
        throw new Error(`Zone ${zone.name} member ${memberIndex} has an unsupported role or look.`);
      }
      if (member.peakPercent !== undefined && typeof member.peakPercent !== "number") {
        throw new Error(`Zone ${zone.name} member ${memberIndex} peakPercent must be a number.`);
      }
      return {
        role: member.role as LightRole,
        look: member.look as RuntimeLook,
        ...(member.peakPercent === undefined ? {} : { peakPercent: member.peakPercent as number }),
      };
    });
    if (zone.buttonRole !== undefined && zone.buttonRole !== "Button") {
      throw new Error(`Zone ${zone.name} has an unsupported button role.`);
    }
    let weather: RuntimeProfileWeather | undefined;
    if (zone.weather !== undefined) {
      if (!zone.weather || typeof zone.weather !== "object") {
        throw new Error(`Zone ${zone.name} weather must be an object.`);
      }
      const source = zone.weather as Record<string, unknown>;
      const readWeatherRoles = (field: string): LightRole[] => {
        const roles = source[field] ?? [];
        if (!Array.isArray(roles) ||
            roles.some((role) => typeof role !== "string" || !knownRoles.has(role as LightRole))) {
          throw new Error(`Zone ${zone.name} weather.${field} must contain supported light roles.`);
        }
        return roles as LightRole[];
      };
      weather = {
        precipitation: readWeatherRoles("precipitation"),
        alerts: readWeatherRoles("alerts"),
      };
    }
    return {
      name: zone.name,
      startMinute: zone.startMinute,
      endMinute: zone.endMinute,
      ...(zone.buttonRole === undefined ? {} : { buttonRole: "Button" as const }),
      members,
      ...(weather === undefined ? {} : { weather }),
    };
  });
  return { schemaVersion: 1, zones };
}

function withCString<T>(module: SunsimModule, value: string, use: (pointer: number) => T): T {
  const size = module.lengthBytesUTF8(value) + 1;
  const pointer = module._malloc(size);
  if (pointer === 0) throw new Error("Could not allocate memory for the installation profile.");
  try {
    module.stringToUTF8(value, pointer, size);
    return use(pointer);
  } finally {
    module._free(pointer);
  }
}

function applyRuntimeProfile(module: SunsimModule, profile: RuntimeInstallationProfile): void {
  if (profile.schemaVersion !== 1 || profile.zones.length < 1 || profile.zones.length > 8) {
    throw new Error("Installation profile must be schema version 1 and contain 1–8 zones.");
  }
  const zoneNames = new Set<string>();
  const assignedRoles = new Set<LightRole>();
  const roleZones = new Map<LightRole, number>();
  let buttonZone = -1;
  for (const [zoneIndex, zone] of profile.zones.entries()) {
    if (!zone.name || zone.name.length > 31 || zoneNames.has(zone.name)) {
      throw new Error(`Invalid or duplicate installation zone name: ${zone.name}`);
    }
    if (!Number.isInteger(zone.startMinute) || !Number.isInteger(zone.endMinute) ||
        zone.startMinute < 0 || zone.startMinute >= zone.endMinute || zone.endMinute > 1440) {
      throw new Error(`Zone ${zone.name} must have a valid non-empty daily time window.`);
    }
    if (zone.members.length === 0) throw new Error(`Zone ${zone.name} has no light members.`);
    zoneNames.add(zone.name);
    if (zone.buttonRole !== undefined) {
      if (zone.buttonRole !== "Button" || buttonZone !== -1) {
        throw new Error("The supported Button can control only one zone.");
      }
      buttonZone = zoneIndex;
    }
    for (const member of zone.members) {
      if (assignedRoles.has(member.role)) throw new Error(`Role ${member.role} is assigned more than once.`);
      if (member.look === "on-off" && member.role !== "Desk" && member.role !== "Grow") {
        throw new Error(`${member.role} cannot use the on/off-only look.`);
      }
      if (member.look !== "on-off" && (member.role === "Desk" || member.role === "Grow")) {
        throw new Error(`${member.role} requires the on/off-only look.`);
      }
      const peak = member.peakPercent ?? 100;
      if (!Number.isInteger(peak) || peak < 1 || peak > 100) {
        throw new Error(`Peak intensity for ${member.role} must be between 1 and 100 percent.`);
      }
      assignedRoles.add(member.role);
      roleZones.set(member.role, zoneIndex);
    }
  }
  if (assignedRoles.size !== 8) {
    throw new Error("Installation profile must assign all eight supported lights exactly once.");
  }
  for (const [zoneIndex, zone] of profile.zones.entries()) {
    const weatherRoles = [
      ...(zone.weather?.precipitation ?? []),
      ...(zone.weather?.alerts ?? []),
    ];
    for (const role of weatherRoles) {
      if (roleZones.get(role) !== zoneIndex || role === "Desk" || role === "Grow") {
        throw new Error(`Weather role ${role} must be a color light assigned to zone ${zone.name}.`);
      }
    }
  }
  if (module._sunsim_profile_begin(profile.zones.length) === 0) {
    throw new Error("The WebAssembly runtime could not begin the installation profile.");
  }
  profile.zones.forEach((zone, zoneIndex) => {
    const accepted = withCString(module, zone.name, (name) =>
      module._sunsim_profile_zone(zoneIndex, name, zone.startMinute, zone.endMinute),
    );
    if (accepted === 0) throw new Error(`The WebAssembly runtime rejected zone ${zone.name}.`);
    for (const member of zone.members) {
      const result = module._sunsim_profile_member(
        zoneIndex,
        roleIndices[member.role],
        lookIndices[member.look],
        member.peakPercent ?? 100,
      );
      if (result === 0) throw new Error(`The WebAssembly runtime rejected role ${member.role}.`);
    }
    if (zone.buttonRole) {
      if (module._sunsim_profile_button(zoneIndex, roleIndices[zone.buttonRole]) === 0) {
        throw new Error(`The WebAssembly runtime rejected button assignment for ${zone.name}.`);
      }
    }
    const weather = [
      [0, zone.weather?.precipitation ?? []],
      [1, zone.weather?.alerts ?? []],
    ] as const;
    for (const [effect, roles] of weather) {
      for (const role of roles) {
        if (module._sunsim_profile_weather(zoneIndex, roleIndices[role], effect) === 0) {
          throw new Error(`The WebAssembly runtime rejected weather assignment for ${role}.`);
        }
      }
    }
  });
  if (module._sunsim_profile_finish() === 0) {
    throw new Error("The WebAssembly runtime rejected the installation topology.");
  }
}

export async function createSimulatorController(
  profile?: RuntimeInstallationProfile,
): Promise<SimulatorController> {
  const { default: createSunsim } = await import("../sunsim_web_simulator.js");
  const module = await createSunsim();
  if (profile) applyRuntimeProfile(module, parseRuntimeInstallationProfile(profile));
  return new WasmSimulatorController(module);
}
