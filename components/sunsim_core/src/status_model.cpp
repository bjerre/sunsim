#include "sunsim/core/status_model.h"

#include <memory>

#include "sunsim/core/day_info.h"
#include "sunsim/core/daylight.h"

namespace sunsim::core {

namespace {
// Bulbs are pinged every 60 s, so a couple of missed replies before "silent".
constexpr int64_t kOnlineWindowUs = 150LL * 1000 * 1000;
} // namespace

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
    // (it posts one every Tick), so links reflect this second, not the last.
    snap_.uptime_us = e.uptime_us;
    snap_.time      = e.time;
}

void StatusModel::publish()
{
    const ev::Tick e{.uptime_us = snap_.uptime_us, .time = snap_.time};
    snap_.day = computeDayInfo(e.time.local, e.time.utc_offset_hours, snap_.config.latitude_deg,
                               snap_.config.longitude_deg);
    snap_.in_active_window = DaylightSchedule{snap_.config}.inActiveWindow(e.time.local);

    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        const ev::DeviceStatus &d = snap_.devices[i];
        if (!d.paired) {
            snap_.links[i] = ev::DeviceLink::Unpaired;
        } else if (d.reachable && d.last_heard_us != 0 && e.uptime_us - d.last_heard_us <= kOnlineWindowUs) {
            snap_.links[i] = ev::DeviceLink::Online;
        } else {
            snap_.links[i] = ev::DeviceLink::Silent;
        }
    }
    post_(ev::StatusChanged{std::make_shared<const ev::StatusSnapshot>(snap_)});
}

} // namespace sunsim::core
