#pragma once

#include <chrono>
#include <cmath>
#include <ctime>

#include "sunsim/ev/types.h"

namespace sunsim::platform {

// Compresses a day into a short, continuously-looping real-time window - for
// bench-testing the daylight program without waiting for an actual
// sunrise/sunset. Construct once; now() reflects elapsed real time since
// construction.
//
// Only the active window is simulated (ev::ProgramConfig's active
// hours and days, read live on each call so `config` changes show up on the
// next loop): each loop runs from active_start_hour to active_end_hour, and
// successive loops step through the active days only (starting Monday), so
// no loop time is spent on hours or weekends where the lights are off
// anyway. The date stays January 1.
class FastDayClock {
public:
    explicit FastDayClock(std::chrono::seconds real_seconds_per_day) noexcept
        : real_seconds_per_day_(real_seconds_per_day), start_(std::chrono::steady_clock::now())
    {
    }

    [[nodiscard]] std::tm now(const ev::ProgramConfig &config) const noexcept
    {
        const auto   elapsed      = std::chrono::steady_clock::now() - start_;
        const double day_seconds  = std::chrono::duration<double>(real_seconds_per_day_).count();
        const double elapsed_secs = std::chrono::duration<double>(elapsed).count();
        const double frac_of_loop = std::fmod(elapsed_secs, day_seconds) / day_seconds;
        const auto   loop_index   = static_cast<long>(elapsed_secs / day_seconds);

        const double hour_of_day =
            config.active_start_hour + frac_of_loop * (config.active_end_hour - config.active_start_hour);

        std::tm tm{};
        tm.tm_wday               = activeWeekday(config.active_days, loop_index);
        tm.tm_hour               = static_cast<int>(hour_of_day);
        const double minute_frac = (hour_of_day - tm.tm_hour) * 60.0;
        tm.tm_min                = static_cast<int>(minute_frac);
        tm.tm_sec                = static_cast<int>((minute_frac - tm.tm_min) * 60.0);
        return tm;
    }

private:
    // The loop_index-th active day (tm_wday numbering), cycling Monday-first.
    [[nodiscard]] static int activeWeekday(uint8_t active_days, long loop_index) noexcept
    {
        int active[7];
        int count = 0;
        for (int i = 0; i < 7; ++i) {
            const int day = (i + 1) % 7; // Monday first
            if ((active_days >> day) & 1U) {
                active[count++] = day;
            }
        }
        return count ? active[loop_index % count] : 1;
    }

    std::chrono::seconds                  real_seconds_per_day_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace sunsim::platform
