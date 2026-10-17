export module sunsim.core:sky;

import std.compat;
import sunsim.ev;

export namespace sunsim::core {

// The forecast's cloud cover at `time`, 0 (clear) to 1 (overcast),
// interpolated between its hours; nullopt without data for then. Fast-day
// mode's simulated day reads the forecast's first day, so the sped-up day
// shows today's sky. Pure, like :daylight and :solar.
[[nodiscard]] std::optional<double> cloudCover(const ev::CloudCover &forecast, const ev::TimeReading &time) noexcept;

// The cloud cover now, and where it came from.
struct CloudReading {
    double cover = 0.0;   // 0 (clear) to 1 (overcast)
    bool   live  = false; // the live conditions, not the forecast
};

// Everything known about the weather: the hourly forecast, and the live
// conditions and alerts, which count only while fresh (kWeatherFreshFor).
// Each module that needs it keeps one, fed from the events; `now` is the
// latest Tick's.
class Weather {
public:
    void onForecast(const ev::CloudCoverUpdated &e) noexcept { forecast_ = e.forecast; }
    void onConditions(const ev::ConditionsUpdated &e) noexcept { conditions_ = e; }
    void onAlerts(const ev::AlertsUpdated &e) noexcept { alerts_ = e; }

    [[nodiscard]] std::optional<ev::Conditions> conditions(ev::Instant now) const noexcept;

    // The alerts in force at `now` (not ended), highest level first.
    [[nodiscard]] ev::Alerts alerts(ev::Instant now) const noexcept;
    [[nodiscard]] ev::AlertLevel alertLevel(ev::Instant now) const noexcept;

    // The cloud cover: the live reading while it's fresh, else the
    // forecast's at `time`. Fast-day's simulated day always reads the
    // forecast. nullopt: neither has data.
    [[nodiscard]] std::optional<CloudReading> cloud(const ev::TimeReading &time, ev::Instant now) const noexcept;

private:
    [[nodiscard]] static bool fresh(ev::Instant at, ev::Instant now) noexcept
    {
        return at <= now && now - at < ev::kWeatherFreshFor;
    }

    ev::CloudCover                       forecast_{};
    std::optional<ev::ConditionsUpdated> conditions_;
    std::optional<ev::AlertsUpdated>     alerts_;
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
std::chrono::sys_days dateOf(int year, int yday) noexcept
{
    return std::chrono::sys_days{std::chrono::year{year} / 1 / 1} + std::chrono::days{yday};
}
} // namespace

std::optional<double> cloudCover(const ev::CloudCover &forecast, const ev::TimeReading &time) noexcept
{
    if (forecast.year == 0 || !time.valid) {
        return std::nullopt;
    }
    const long days = time.simulated ? 0
                                     : (dateOf(time.local.tm_year + 1900, time.local.tm_yday) -
                                        dateOf(forecast.year, forecast.yday))
                                           .count();
    const double hour =
        days * 24.0 + time.local.tm_hour + time.local.tm_min / 60.0 + time.local.tm_sec / 3600.0;
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

std::optional<ev::Conditions> Weather::conditions(ev::Instant now) const noexcept
{
    if (!conditions_ || !fresh(conditions_->at, now)) {
        return std::nullopt;
    }
    return conditions_->now;
}

ev::Alerts Weather::alerts(ev::Instant now) const noexcept
{
    ev::Alerts in_force;
    if (!alerts_ || !fresh(alerts_->at, now)) {
        return in_force;
    }
    for (const ev::Alert &alert : alerts_->alerts.list()) {
        if (alert.until > now) {
            in_force.items[in_force.count++] = alert;
        }
    }
    return in_force;
}

ev::AlertLevel Weather::alertLevel(ev::Instant now) const noexcept
{
    const ev::Alerts in_force = alerts(now);
    return in_force.count > 0 ? in_force.items[0].level : ev::AlertLevel::None;
}

std::optional<CloudReading> Weather::cloud(const ev::TimeReading &time, ev::Instant now) const noexcept
{
    if (const auto current = conditions(now); current && !time.simulated) {
        return CloudReading{.cover = current->cloud_percent / 100.0, .live = true};
    }
    return cloudCover(forecast_, time).transform([](double cover) { return CloudReading{.cover = cover}; });
}

} // namespace sunsim::core
