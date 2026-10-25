interface SunsimModule {
  UTF8ToString(pointer: number): string;
  _sunsim_init(): void;
  _sunsim_advance(seconds: number): void;
  _sunsim_set_minute(minute: number): void;
  _sunsim_set_location(latitude: number, longitude: number): number;
  _sunsim_set_clock(
    year: number,
    month: number,
    day: number,
    hour: number,
    minute: number,
    utcOffsetMinutes: number,
  ): number;
  _sunsim_press(kind: number): void;
  _sunsim_weather(precipitation: number, cloud: number): void;
  _sunsim_alert(level: number): void;
  _sunsim_state(): number;
}

declare module "*sunsim_build2_smoke.js" {
  const createSunsim: () => Promise<SunsimModule>;
  export default createSunsim;
}
