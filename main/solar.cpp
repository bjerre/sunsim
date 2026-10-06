#include "solar.h"

#include <cmath>

namespace sunsim {

namespace {

constexpr double kDeg2Rad = M_PI / 180.0;
constexpr double kRad2Deg = 180.0 / M_PI;

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

} // namespace sunsim
