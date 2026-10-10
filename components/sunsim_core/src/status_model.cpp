module;

#include "sunsim/ev/events.h"

module sunsim.core;

import std;

namespace sunsim::core {

StatusModel::StatusModel(ev::Post post) : post_(std::move(post)) {}

void StatusModel::onConfig(const ev::ConfigChanged &e)
{
    snap_.config = e.config;
    snap_.peaks  = e.peaks;
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
}

void StatusModel::publish()
{
    const ev::TimeReading &t = snap_.time;
    snap_.day = computeDayInfo(t.local, t.utc_offset_hours, snap_.config.latitude_deg, snap_.config.longitude_deg);
    snap_.in_active_window = DaylightSchedule{snap_.config}.inActiveWindow(t.local);
    post_(ev::StatusChanged{std::make_shared<const ev::StatusSnapshot>(snap_)});
}

} // namespace sunsim::core
