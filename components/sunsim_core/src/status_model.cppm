export module sunsim.core:status_model;

import std.compat;
import sunsim.ev;
import :day_info;
import :gate;

export namespace sunsim::core {

// Gathers what the status display and console show into one snapshot,
// published as StatusChanged once a second (after each Tick's DevicesUpdated).
class StatusModel {
public:
    explicit StatusModel(ev::Post post);

    void on(const ev::ConfigChanged &e);
    void on(const ev::ModeChanged &e);
    void on(const ev::DevicesUpdated &e);
    void on(const ev::Tick &e);
    void on(const ev::SkyChanged &e) { snap_.sky = e.sky; }

private:
    void publish();

    ev::Post           post_;
    ev::StatusSnapshot snap_{};
};

} // namespace sunsim::core

namespace sunsim::core {

StatusModel::StatusModel(ev::Post post) : post_(std::move(post)) {}

void StatusModel::on(const ev::ConfigChanged &e)
{
    snap_.config     = e.config;
    snap_.peaks      = e.peaks;
    snap_.min_levels = e.min_levels;
}

void StatusModel::on(const ev::ModeChanged &e)
{
    snap_.zones[e.zone].mode = e.mode;
}

void StatusModel::on(const ev::DevicesUpdated &e)
{
    snap_.devices = e.devices;
    snap_.records = e.records;
    snap_.armed   = e.armed;
    publish();
}

void StatusModel::on(const ev::Tick &e)
{
    // Published once the registry's DevicesUpdated for this tick arrives
    // (it posts one every Tick), so device links reflect this second, not the last.
    snap_.at   = e.at;
    snap_.time = e.time;
}

void StatusModel::publish()
{
    const ev::TimeReading &t = snap_.time;
    snap_.day = computeDayInfo(t, snap_.config.latitude_deg, snap_.config.longitude_deg);
    for (const ev::ZoneId zone : ev::kZoneIds) {
        snap_.zones[zone].gate_open = openness(ev::spec(zone).gate, t, snap_.config) > 0.0;
    }
    post_(ev::StatusChanged{std::make_shared<const ev::StatusSnapshot>(snap_)});
}

} // namespace sunsim::core
