interface SunsimModule {
  UTF8ToString(pointer: number): string;
  lengthBytesUTF8(text: string): number;
  stringToUTF8(text: string, outPtr: number, maxBytesToWrite: number): number;
  _malloc(size: number): number;
  _free(pointer: number): void;
  _sunsim_init(): void;
  _sunsim_profile_begin(zoneCount: number): number;
  _sunsim_profile_zone(index: number, name: number, startMinute: number, endMinute: number): number;
  _sunsim_profile_member(zoneIndex: number, roleIndex: number, lookIndex: number, peakPercent: number): number;
  _sunsim_profile_button(zoneIndex: number, roleIndex: number): number;
  _sunsim_profile_weather(zoneIndex: number, roleIndex: number, effect: number): number;
  _sunsim_profile_finish(): number;
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

declare module "*sunsim_web_simulator.js" {
  const createSunsim: () => Promise<SunsimModule>;
  export default createSunsim;
}
