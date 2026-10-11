// Text formatting shared by the console and the status display.
module sunsim.platform:format; // internal: not exported

import std.compat;

namespace sunsim::platform {

// In tm_wday order.
inline constexpr const char *kWeekdayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Fractional hours as "HH:MM". 24:00 stays as is (a window ending at
// midnight); anything later wraps into the next day.
inline void formatClock(char *buf, std::size_t len, double hours)
{
    long minutes = std::lround(hours * 60.0);
    if (minutes > 24 * 60) {
        minutes -= 24 * 60;
    }
    std::snprintf(buf, len, "%02ld:%02ld", minutes / 60, minutes % 60);
}

// Fractional hours as "Xh MMm".
inline void formatDuration(char *buf, std::size_t len, double hours)
{
    const long minutes = std::lround(hours * 60.0);
    std::snprintf(buf, len, "%ldh %02ldm", minutes / 60, minutes % 60);
}

} // namespace sunsim::platform
