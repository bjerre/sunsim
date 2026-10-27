module;

#include <time.h> // localtime_r (POSIX)

module sunsim.platform:zone; // internal: not exported

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif

namespace sunsim::platform {

// The local civil UTC offset (DST included) at `at`, from the POSIX time
// zone (TZ, set at boot): localtime_r's calendar fields read as if they were
// UTC, less the instant itself. (No portable tm_gmtoff; mktime() on
// gmtime()'s fields would be off by the DST hour, as gmtime() sets
// tm_isdst = 0.) The one place that touches std::tm.
inline std::chrono::minutes utcOffset(std::chrono::sys_seconds at) noexcept
{
    using namespace std::chrono;
    const std::time_t c = system_clock::to_time_t(at);
    std::tm           local{};
    localtime_r(&c, &local);
    const sys_seconds as_if_utc = sys_days{year{local.tm_year + 1900} / (local.tm_mon + 1) / local.tm_mday} +
                                  hours{local.tm_hour} + minutes{local.tm_min} + seconds{local.tm_sec};
    return round<minutes>(as_if_utc - at);
}

// The wall-clock time here at `at`.
inline std::chrono::local_seconds toLocal(std::chrono::sys_seconds at) noexcept
{
    return std::chrono::local_seconds{at.time_since_epoch()} + utcOffset(at);
}

} // namespace sunsim::platform
