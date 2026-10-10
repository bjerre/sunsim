module sunsim.core;

import std;

namespace sunsim::core {

namespace {

constexpr double kDeg2Rad = std::numbers::pi / 180.0;
constexpr double kRad2Deg = 180.0 / std::numbers::pi;

// Horizon (90 deg) plus ~50' of atmospheric refraction plus the sun's
// apparent radius: the standard zenith angle used to define sunrise/sunset.
constexpr double kZenithDeg = 90.833;

double normalizeDegrees(double deg) noexcept
{
    deg = std::fmod(deg, 360.0);
    return deg < 0.0 ? deg + 360.0 : deg;
}

double normalizeHours(double hours) noexcept
{
    hours = std::fmod(hours, 24.0);
    return hours < 0.0 ? hours + 24.0 : hours;
}

double sunEventLocalHour(double latitude_deg, double longitude_deg, int day_of_year, double utc_offset_hours,
                          bool is_sunrise) noexcept
{
    const double lng_hour  = longitude_deg / 15.0;
    const double base_hour = is_sunrise ? 6.0 : 18.0;
    const double t         = day_of_year + ((base_hour - lng_hour) / 24.0);

    const double m = (0.9856 * t) - 3.289;

    double l = m + (1.916 * std::sin(m * kDeg2Rad)) + (0.020 * std::sin(2.0 * m * kDeg2Rad)) + 282.634;
    l        = normalizeDegrees(l);

    double       ra          = kRad2Deg * std::atan(0.91764 * std::tan(l * kDeg2Rad));
    ra                       = normalizeDegrees(ra);
    const double l_quadrant  = std::floor(l / 90.0) * 90.0;
    const double ra_quadrant = std::floor(ra / 90.0) * 90.0;
    ra                       = (ra + (l_quadrant - ra_quadrant)) / 15.0; // hours

    const double sin_dec = 0.39782 * std::sin(l * kDeg2Rad);
    const double cos_dec = std::cos(std::asin(sin_dec));

    double cos_h = (std::cos(kZenithDeg * kDeg2Rad) - (sin_dec * std::sin(latitude_deg * kDeg2Rad))) /
                   (cos_dec * std::cos(latitude_deg * kDeg2Rad));
    // Polar day/night: clamp instead of leaving an acos() domain error, so
    // this degenerates to a zero- or full-length window for the caller's
    // brightness curve rather than propagating NaN into bulb commands.
    if (cos_h > 1.0) {
        cos_h = 1.0;
    } else if (cos_h < -1.0) {
        cos_h = -1.0;
    }

    const double h_deg = is_sunrise ? (360.0 - kRad2Deg * std::acos(cos_h)) : (kRad2Deg * std::acos(cos_h));
    const double h     = h_deg / 15.0; // hours

    const double time_utc = h + ra - (0.06571 * t) - 6.622 - lng_hour;
    return normalizeHours(time_utc + utc_offset_hours);
}

} // namespace

SunTimes computeSunTimes(double latitude_deg, double longitude_deg, int day_of_year, double utc_offset_hours) noexcept
{
    return {.sunrise_hour = sunEventLocalHour(latitude_deg, longitude_deg, day_of_year, utc_offset_hours, true),
            .sunset_hour  = sunEventLocalHour(latitude_deg, longitude_deg, day_of_year, utc_offset_hours, false)};
}

SunPosition computeSunPosition(double latitude_deg, double longitude_deg, int day_of_year, double local_hour,
                               double utc_offset_hours) noexcept
{
    const double utc_hour = local_hour - utc_offset_hours;

    // Fractional year (radians).
    const double g = 2.0 * std::numbers::pi / 365.0 * (day_of_year - 1 + (utc_hour - 12.0) / 24.0);

    const double eq_time_min = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g) -
                                         0.014615 * std::cos(2 * g) - 0.040849 * std::sin(2 * g));
    const double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) - 0.006758 * std::cos(2 * g) +
                        0.000907 * std::sin(2 * g) - 0.002697 * std::cos(3 * g) + 0.00148 * std::sin(3 * g);

    // True solar time (minutes) -> hour angle (0 at solar noon, negative in the morning).
    const double true_solar_min = utc_hour * 60.0 + eq_time_min + 4.0 * longitude_deg;
    const double hour_angle     = (true_solar_min / 4.0 - 180.0) * kDeg2Rad;

    const double lat       = latitude_deg * kDeg2Rad;
    const double cos_zen   = std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(hour_angle);
    const double elevation = 90.0 - std::acos(std::fmax(-1.0, std::fmin(1.0, cos_zen))) * kRad2Deg;

    // Measured from South, positive toward West; +180 makes it a compass bearing.
    const double from_south = std::atan2(std::sin(hour_angle),
                                         std::cos(hour_angle) * std::sin(lat) - std::tan(decl) * std::cos(lat));
    return {.azimuth_deg = normalizeDegrees(from_south * kRad2Deg + 180.0), .elevation_deg = elevation};
}

} // namespace sunsim::core
