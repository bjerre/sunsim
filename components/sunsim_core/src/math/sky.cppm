export module sunsim.core:sky;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::core {

// The forecast's cloud cover at `time`, 0 (clear) to 1 (overcast),
// interpolated between its hours; nullopt without data for then. Fast-day
// mode's simulated day reads the forecast's first day, so the sped-up day
// shows today's sky. Pure, like :daylight and :solar.
[[nodiscard]] std::optional<double> cloudCover(const ev::CloudCover &forecast, const ev::TimeReading &time) noexcept;

} // namespace sunsim::core

namespace sunsim::core {

std::optional<double> cloudCover(const ev::CloudCover &forecast, const ev::TimeReading &time) noexcept
{
    if (!forecast.day || !time.valid) {
        return std::nullopt;
    }
    // Hours into the forecast; fast-day's simulated day reads its first day.
    const ev::Hours into{time.simulated ? time.sinceMidnight() : time.local - *forecast.day};
    const double    hour = into.count();
    if (hour < 0.0 || hour > ev::CloudCover::kHours - 1.0) {
        return std::nullopt;
    }

    const auto    i    = static_cast<std::size_t>(hour);
    const uint8_t here = forecast.percent[i];
    const uint8_t next = i + 1 < ev::CloudCover::kHours ? forecast.percent[i + 1] : here;
    constexpr uint8_t kNoData = ev::CloudCover::kNoData;
    if (here == kNoData && next == kNoData) {
        return std::nullopt;
    }
    if (here == kNoData || next == kNoData) {
        return (here == kNoData ? next : here) / 100.0;
    }
    return std::lerp(here, next, hour - i) / 100.0;
}

} // namespace sunsim::core
