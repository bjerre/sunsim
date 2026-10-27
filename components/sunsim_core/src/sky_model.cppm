export module sunsim.core:sky_model;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :sky;

export namespace sunsim::core {

// What the sky is now, from the forecast (daily) and the live conditions
// and alerts (every 15 minutes), which count only while fresh
// (kWeatherFreshFor) and, for alerts, until they end. Re-judged on every
// Tick and each update; posts SkyChanged whenever the conclusion changes,
// so nobody else keeps the weather or a clock for it.
//
// The cloud cover is the live reading while it's fresh, else the
// forecast's for the time; fast-day's simulated day always reads the
// forecast.
class SkyModel {
public:
    explicit SkyModel(ev::Post post) : post_(std::move(post)) {}

    void on(const ev::Tick &e);
    void on(const ev::CloudCoverUpdated &e);
    void on(const ev::ConditionsUpdated &e);
    void on(const ev::AlertsUpdated &e);

private:
    void                  update();
    [[nodiscard]] ev::Sky judge() const;
    [[nodiscard]] bool    fresh(ev::Instant at) const noexcept { return now_ - at < ev::kWeatherFreshFor; }

    ev::Post                             post_;
    ev::CloudCover                       forecast_{};
    std::optional<ev::ConditionsUpdated> conditions_;
    std::optional<ev::AlertsUpdated>     alerts_;
    ev::Instant                          now_{};  // the latest Tick's
    ev::TimeReading                      time_{}; // likewise
    std::optional<ev::Sky>               posted_;
};

} // namespace sunsim::core

namespace sunsim::core {

void SkyModel::on(const ev::Tick &e)
{
    now_  = e.at;
    time_ = e.time;
    update();
}

void SkyModel::on(const ev::CloudCoverUpdated &e)
{
    forecast_ = e.forecast;
    update();
}

void SkyModel::on(const ev::ConditionsUpdated &e)
{
    conditions_ = e;
    update();
}

void SkyModel::on(const ev::AlertsUpdated &e)
{
    alerts_ = e;
    update();
}

void SkyModel::update()
{
    const ev::Sky sky = judge();
    if (sky != posted_) {
        posted_ = sky;
        post_(ev::SkyChanged{sky});
    }
}

ev::Sky SkyModel::judge() const
{
    ev::Sky sky;
    if (conditions_ && fresh(conditions_->at)) {
        sky.conditions = conditions_->now;
    }
    if (sky.conditions && !time_.simulated) {
        sky.cloud_percent = sky.conditions->cloud_percent;
        sky.cloud_live    = true;
    } else if (const auto cover = cloudCover(forecast_, time_)) {
        sky.cloud_percent = static_cast<uint8_t>(std::lround(*cover * 100.0));
    }
    if (alerts_ && fresh(alerts_->at)) {
        for (const ev::Alert &alert : alerts_->alerts.list()) {
            if (alert.until > now_) {
                sky.alerts.items[sky.alerts.count++] = alert;
            }
        }
    }
    return sky;
}

} // namespace sunsim::core
