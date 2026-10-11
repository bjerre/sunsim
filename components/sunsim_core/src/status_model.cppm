export module sunsim.core:status_model;

import std.compat;
import sunsim.ev;
import :day_info;
import :daylight;
import :sky;

export namespace sunsim::core {

// Gathers what the status display and console show into one snapshot,
// published as StatusChanged once a second (after each Tick's DevicesUpdated).
class StatusModel {
public:
    explicit StatusModel(ev::Post post);

    void onConfig(const ev::ConfigChanged &e);
    void onModeChanged(const ev::ModeChanged &e);
    void onDevices(const ev::DevicesUpdated &e);
    void onTick(const ev::Tick &e);
    void onCloudCover(const ev::CloudCoverUpdated &e);

private:
    void publish();

    ev::Post           post_;
    ev::StatusSnapshot snap_{};
    ev::CloudCover     forecast_{};
};

} // namespace sunsim::core

namespace sunsim::core {

StatusModel::StatusModel(ev::Post post) : post_(std::move(post)) {}

void StatusModel::onConfig(const ev::ConfigChanged &e)
{
    snap_.config     = e.config;
    snap_.peaks      = e.peaks;
    snap_.min_levels = e.min_levels;
}

void StatusModel::onCloudCover(const ev::CloudCoverUpdated &e)
{
    forecast_ = e.forecast;
}

void StatusModel::onModeChanged(const ev::ModeChanged &e)
{
    snap_.mode = e.mode;
}

void StatusModel::onDevices(const ev::DevicesUpdated &e)
{
    snap_.devices = e.devices;
    snap_.records = e.records;
    snap_.armed   = e.armed;
    publish();
}

void StatusModel::onTick(const ev::Tick &e)
{
    // Published once the registry's DevicesUpdated for this tick arrives
    // (it posts one every Tick), so device links reflect this second, not the last.
    snap_.at   = e.at;
    snap_.time = e.time;
    const auto cloud    = cloudCover(forecast_, e.time);
    snap_.cloud_percent = cloud ? std::optional<uint8_t>{static_cast<uint8_t>(std::lround(*cloud * 100.0))}
                                : std::nullopt;
}

void StatusModel::publish()
{
    const ev::TimeReading &t = snap_.time;
    snap_.day = computeDayInfo(t.local, t.utc_offset_hours, snap_.config.latitude_deg, snap_.config.longitude_deg);
    snap_.in_active_window = DaylightSchedule{snap_.config}.inActiveWindow(t.local);
    post_(ev::StatusChanged{std::make_shared<const ev::StatusSnapshot>(snap_)});
}

} // namespace sunsim::core
