import type { LightColor, SimLight, SimulatorState } from "./simulator.js";

type Rgb = [number, number, number];

function temperatureRgb(mireds: number): Rgb {
  const kelvin = Math.max(1800, Math.min(6500, 1_000_000 / Math.max(1, mireds)));
  const t = (kelvin - 1800) / (6500 - 1800);
  return [
    Math.round(255 - 75 * t),
    Math.round(183 + 53 * t),
    Math.round(105 + 126 * t),
  ];
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

export function colorCss(color: LightColor): string {
  if (color.mode === "hue") {
    return `hsl(${Math.round(color.hue * 360 / 254)} ${Math.round(color.saturation * 100 / 254)}% 58%)`;
  }
  return `rgb(${colorRgb(color).join(" ")})`;
}

export function wallLightView(light: SimLight) {
  const color = colorCss(light.color);
  const intensity = Math.max(0, Math.min(1, light.level / 254));
  return {
    color,
    label: light.on ? `${Math.round(light.level / 254 * 100)}%` : "OFF",
    uplightOpacity: light.on ? 0.15 + intensity * 0.8 : 0,
    bulbBackground: light.on
      ? `radial-gradient(circle at 35% 30%, #fff8, ${color} 48%, #514738)`
      : "#29332b",
    bulbFilter: light.on ? `brightness(${0.15 + intensity * 0.85})` : "",
    bulbBoxShadow: light.on
      ? `0 0 ${8 + intensity * 24}px color-mix(in srgb, ${color} 67%, transparent), 0 0 ${20 + intensity * 32}px color-mix(in srgb, ${color} 33%, transparent)`
      : "none",
  };
}

export function torchiereView(light: SimLight, mode: string) {
  const color = colorCss(light.color);
  return {
    color,
    opacity: light.on ? 0.2 + Math.max(0.16, light.level / 254) * 0.65 : 0,
    status: light.on
      ? `ON · ${Math.round(light.level / 254 * 100)}% · ${Math.round(1_000_000 / (light.color.mode === "mireds" ? light.color.value : 250))} K`
      : mode === "off" ? "OFF · manual override" : "OFF · outside work schedule",
    shadeBackground: light.on ? color : "",
    filter: light.on
      ? `drop-shadow(0 0 10px rgb(${colorRgb(light.color).join(" ")} / 55%))`
      : "",
  };
}

export function windowView(state: SimulatorState) {
  const relativeAzimuth = ((state.sun.azimuthDeg - 180 + 540) % 360) - 180;
  const elevation = Math.max(0, Math.min(90, state.sun.elevationDeg));
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
  const direction = relativeAzimuth < -15 ? "east of south"
    : relativeAzimuth > 15 ? "west of south" : "south";

  return {
    left: `${50 + relativeAzimuth / 180 * 100}%`,
    top: `${100 - elevation / 90 * 100}%`,
    visible: state.sun.elevationDeg > 0 && Math.abs(relativeAzimuth) <= 90,
    brightness: 0.15 + 0.85 * Math.sin(elevation * Math.PI / 180),
    tint: `rgb(${tint.join(" ")})`,
    label: state.sun.elevationDeg > 0
      ? `${Math.round(state.sun.elevationDeg)}° · ${direction}`
      : "Below horizon",
  };
}
