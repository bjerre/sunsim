#pragma once

namespace sunsim {

struct SunTimes {
    double sunrise_hour; // local civil time, fractional hours, normalized to [0,24)
    double sunset_hour;  // local civil time, fractional hours, normalized to [0,24)
};

// Local civil-time sunrise/sunset for a given latitude/longitude and day of
// year, via the "Sunrise/Sunset Algorithm" from the Almanac for Computers,
// 1990 (Nautical Almanac Office, US Naval Observatory).
//
//   latitude_deg      degrees North (negative = South).
//   longitude_deg     degrees East (negative = West).
//   day_of_year       1-366 (e.g. tm_yday + 1).
//   utc_offset_hours  the local civil UTC offset in effect on that day,
//                     including DST, positive = East of Greenwich (e.g.
//                     derive from comparing a known instant's gmtime_r vs.
//                     mktime representation - see LightRig::utcOffsetHours).
//
// At latitudes where the sun doesn't rise/set that day (inside a polar
// circle in local winter/summer), the underlying angle is clamped rather
// than left as a domain error, which degenerates to a same-valued
// sunrise/sunset (zero-length or full-length day) rather than NaN.
[[nodiscard]] SunTimes computeSunTimes(double latitude_deg, double longitude_deg, int day_of_year,
                                        double utc_offset_hours) noexcept;

} // namespace sunsim
