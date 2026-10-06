#include "status.h"

#include "esp_timer.h"

namespace sunsim {

namespace {
// Bulbs are pinged every 60s, so a couple of missed replies before "silent".
constexpr int64_t kOnlineWindowUs = 150LL * 1000 * 1000;
} // namespace

StatusSnapshot takeStatusSnapshot(const LightRig &rig, const ButtonController &button)
{
    StatusSnapshot s;
    s.time        = rig.localTime();
    s.clock_valid = rig.clockValid();
#if CONFIG_SUNSIM_DEBUG_FAST_DAY
    s.simulated = true;
#endif
    const auto &config = rig.scheduleConfig();
    s.day              = computeDayInfo(s.time, rig.utcOffsetHours(), config.latitude_deg, config.longitude_deg);
    s.mode             = button.mode();
    s.in_active_window = rig.inActiveWindow();

    const int64_t now_us = esp_timer_get_time();
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const auto d = rig.deviceStatus(static_cast<Role>(i));
        s.devices[i] = d;
        if (!d.paired) {
            s.links[i] = DeviceLink::Unpaired;
        } else if (d.last_heard_us != 0 && now_us - d.last_heard_us <= kOnlineWindowUs) {
            s.links[i] = DeviceLink::Online;
        } else {
            s.links[i] = DeviceLink::Silent;
        }
    }
    return s;
}

const char *modeName(LightingMode mode) noexcept
{
    switch (mode) {
    case LightingMode::Auto:
        return "daylight";
    case LightingMode::ManualOff:
        return "off";
    case LightingMode::RedAlert:
        return "RED ALERT";
    }
    return "?";
}

} // namespace sunsim
