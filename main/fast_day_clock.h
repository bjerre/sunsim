#pragma once

#include <chrono>
#include <cmath>
#include <ctime>

namespace sunsim {

// Compresses a full 24h day into a short, continuously-looping real-time
// window - for bench-testing the daylight program without waiting for an
// actual sunrise/sunset. Construct once; now() reflects elapsed real time
// since construction, wrapped to a 24h-of-day tm each time it's called.
class FastDayClock {
public:
    explicit FastDayClock(std::chrono::seconds real_seconds_per_day) noexcept
        : real_seconds_per_day_(real_seconds_per_day), start_(std::chrono::steady_clock::now())
    {
    }

    [[nodiscard]] std::tm now() const noexcept
    {
        const auto   elapsed      = std::chrono::steady_clock::now() - start_;
        const double day_seconds  = std::chrono::duration<double>(real_seconds_per_day_).count();
        const double elapsed_secs = std::chrono::duration<double>(elapsed).count();
        const double frac_of_day  = std::fmod(elapsed_secs, day_seconds) / day_seconds;
        const double hour_of_day  = frac_of_day * 24.0;

        std::tm tm{};
        tm.tm_hour              = static_cast<int>(hour_of_day);
        const double minute_frac = (hour_of_day - tm.tm_hour) * 60.0;
        tm.tm_min               = static_cast<int>(minute_frac);
        tm.tm_sec               = static_cast<int>((minute_frac - tm.tm_min) * 60.0);
        return tm;
    }

private:
    std::chrono::seconds                  real_seconds_per_day_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace sunsim
