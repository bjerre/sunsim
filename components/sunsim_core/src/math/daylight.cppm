export module sunsim.core:daylight;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :solar;

export namespace sunsim::core {

// CIE 1931 xy of a blackbody at `kelvin` (Kim et al. cubic spline
// approximation of the Planckian locus, valid 1667-25000K): for colors
// below most bulbs' ~2700K color-temperature floor.
[[nodiscard]] ev::Xy planckianXy(double kelvin) noexcept;

// What a look sees: the settings, the time, and where the light is.
struct LookContext {
    const ev::ProgramConfig &config;
    // Carries the local civil UTC offset in effect (DST included; see
    // platform::Clock for how it's derived), which the sun's position needs.
    ev::TimeReading time;
    // Along its group, as the group is lettered: +1 the first (for the wall:
    // the east end), 0 the middle, -1 the last.
    double position = 0.0;
    double cloud    = 0.0; // the cloud cover now, 0 (clear) to 1 (overcast)
};

// What a look shows while it's on: how bright, as a fraction of the light's
// own range (0: its minimum level, 1: its peak), and in what color.
struct Glow {
    double    brightness = 0.0;
    ev::Color color      = ev::Mireds{};
};

// What each look (ev::Look) shows (nullopt: off): ungated, as if the light
// may be on; the gate decides whether it is. See DESIGN.md for the math.

// The sun's curve for the configured location: warm and dim at the real
// sunrise and sunset, brightest and coolest at solar noon, off at night;
// dimming away from the sun along the wall, amber while the sun is low,
// flattened by cloud. See :solar for the sun's times and position.
[[nodiscard]] std::optional<Glow> show(const ev::SunCurve &curve, const LookContext &ctx) noexcept;

// Cool to warm across the curve's hours, at full brightness.
[[nodiscard]] std::optional<Glow> show(const ev::WorkCurve &curve, const LookContext &ctx) noexcept;

// A plug has no dimming or color; the zone gate controls its on/off state.
[[nodiscard]] std::optional<Glow> show(const ev::OnOffOnly &look, const LookContext &) noexcept;

// Something that can be a Look: it has a show().
template <class L>
concept Showable = requires(const L &look, const LookContext &ctx) {
    { show(look, ctx) } -> std::same_as<std::optional<Glow>>;
};
static_assert([]<class... L>(std::type_identity<std::variant<L...>>) { return (Showable<L> && ...); }(
                  std::type_identity<ev::Look>{}),
              "every ev::Look needs a show()");

// What `look` shows.
[[nodiscard]] inline std::optional<Glow> show(const ev::Look &look, const LookContext &ctx) noexcept
{
    return std::visit([&](const auto &l) { return show(l, ctx); }, look);
}

} // namespace sunsim::core

namespace sunsim::core {

ev::Xy planckianXy(double kelvin) noexcept
{
    const double t  = std::fmin(25000.0, std::fmax(1667.0, kelvin));
    const double t2 = t * t;
    const double t3 = t2 * t;
    double       x  = 0.0;
    if (t <= 4000.0) {
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    } else {
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    }
    const double x2 = x * x;
    const double x3 = x2 * x;
    double       y  = 0.0;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
    return {.x = static_cast<float>(x), .y = static_cast<float>(y)};
}

std::optional<Glow> show(const ev::SunCurve &, const LookContext &ctx) noexcept
{
    const ev::ProgramConfig &c    = ctx.config;
    const ev::TimeReading   &time = ctx.time;

    // Clouds make the light diffuse: dimmer, cooler, no sun direction and
    // no golden glow, each in proportion to the cover.
    const double overcast = c.clouds_enabled ? std::clamp(ctx.cloud, 0.0, 1.0) : 0.0;

    const ev::Hours now{time.sinceMidnight()};
    const ev::Hours utc_offset{time.utc_offset};
    const int       day_of_year = time.dayOfYear();
    const SunTimes  sun = computeSunTimes(c.latitude_deg, c.longitude_deg, day_of_year, utc_offset);

    const double hour    = now.count();
    double       sunrise = sun.sunrise.count();
    double       sunset  = sun.sunset.count();
    if (sunset <= sunrise) {
        // The window spills past local midnight (near-polar summer): unwrap
        // it onto a single increasing scale rather than treating every hour
        // as "outside" it.
        sunset += 24.0;
    }
    double hour_adj = hour;
    if (hour_adj < sunrise) {
        hour_adj += 24.0;
    }

    if (hour_adj < sunrise || hour_adj > sunset) {
        return {}; // off
    }

    const double day_progress = (hour_adj - sunrise) / (sunset - sunrise);
    const double brightness   = std::sin(std::numbers::pi * day_progress); // 0 at edges, 1 at solar noon

    // Sun direction: sin(azimuth) is the sun's east-west position (+1 due
    // East, 0 due South, -1 due West). `away` is how far along the wall the
    // bulb is from it: 0 under the sun, 1 at the opposite end. Bulbs further
    // away dim, by up to sun_shift_percent.
    const SunPosition pos =
        computeSunPosition(c.latitude_deg, c.longitude_deg, day_of_year, now, utc_offset);
    const double sun_x     = std::sin(pos.azimuth_deg * std::numbers::pi / 180.0);
    const double away      = std::fabs(ctx.position - sun_x) / 2.0;
    const double direction = 1.0 - (c.sun_shift_percent / 100.0) * away * (1.0 - overcast);
    const double sky       = 1.0 - (c.cloud_dim_percent / 100.0) * overcast;

    const double sun_mireds = c.warm_mireds - brightness * (c.warm_mireds - c.cool_mireds);
    const auto   mireds     = static_cast<uint16_t>(std::lround(
        sun_mireds + (c.cool_mireds - sun_mireds) * (c.cloud_cool_percent / 100.0) * overcast));

    Glow glow{.brightness = direction * sky * brightness, .color = ev::Mireds{mireds}};

    // The amber glow follows the sun: bulbs further away see it as if it
    // were up to amber_sweep_deg higher, so they turn amber later and less.
    const double amber_elevation = pos.elevation_deg + c.amber_sweep_deg * away;
    if (c.amber_enabled && c.golden_elevation_deg > 0.0 &&
        amber_elevation < c.golden_elevation_deg) {
        // 0 at golden_elevation_deg, 1 at (or below) the horizon.
        const double golden =
            std::fmin(1.0, (c.golden_elevation_deg - amber_elevation) / c.golden_elevation_deg) *
            (1.0 - overcast); // no glow under a gray sky
        const double amber_mireds = 1e6 / c.amber_kelvin;
        const double m            = mireds + golden * (amber_mireds - mireds);
        glow.color                = planckianXy(1e6 / m);
    }
    return glow;
}

std::optional<Glow> show(const ev::WorkCurve &curve, const LookContext &ctx) noexcept
{
    const ev::DaySpan hours = curve.hours(ctx.config);
    const double      cool  = curve.cool(ctx.config);
    const double      warm  = curve.warm(ctx.config);

    // How far through the hours, 0 to 1.
    const double progress = std::clamp(
        std::chrono::duration<double>{ctx.time.sinceMidnight() - hours.start} / (hours.end - hours.start), 0.0, 1.0);
    const double warmth = progress * progress; // ease-in: stays cool through the morning
    const auto   mireds = static_cast<uint16_t>(std::lround(cool + warmth * (warm - cool)));

    return Glow{.brightness = 1.0, .color = ev::Mireds{mireds}};
}

std::optional<Glow> show(const ev::OnOffOnly &, const LookContext &) noexcept
{
    return Glow{.brightness = 1.0};
}

} // namespace sunsim::core
