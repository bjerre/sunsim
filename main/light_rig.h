#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "bulb_endpoint.h"
#include "daylight.h"
#include "roles_store.h"

namespace sunsim {

// Owns the 5 Zigbee devices sunsim has paired (3 wall bulbs, 1 work bulb, 1
// button), persisted manifest-based role assignment (see role_manifest.h),
// and the daylight program.
// There is exactly one LightRig for the process's lifetime (set up by
// SunsimApp); its static ZDO callbacks reach it through a self-pointer,
// the usual pattern for wrapping C callback APIs in a C++ class.
class LightRig {
public:
    // What time it currently is, for the daylight program. Defaults to the
    // real wall clock; swap in a FastDayClock for the debug fast-day mode.
    using ClockFn = std::function<std::tm()>;

    // What the current local civil UTC offset is (hours, including DST,
    // positive = East of Greenwich), for the sunrise/sunset calculation.
    // Defaults to deriving it from the real wall clock + TZ; override for
    // tests the same way as setClock.
    using UtcOffsetFn = std::function<double()>;

    LightRig();
    explicit LightRig(DaylightSchedule schedule);

    void load();

    void setClock(ClockFn clock);
    void setUtcOffsetFn(UtcOffsetFn utc_offset_fn);

    // Until the clock has been set (SNTP), the time is near the 1970 epoch
    // and the schedule would be nonsense, so every bulb is held off. The
    // debug fast-day clock is synthetic and always counts as set.
    void setRequireSyncedClock(bool require) noexcept { require_synced_clock_ = require; }
    [[nodiscard]] bool clockValid() const;

    // The clock and UTC offset the program runs on (simulated in fast-day mode).
    [[nodiscard]] std::tm localTime() const { return now(); }
    [[nodiscard]] double utcOffsetHours() const { return utc_offset_fn_(); }
    [[nodiscard]] bool inActiveWindow() const { return schedule_.inActiveWindow(now()); }

    // How often applyAllBulbs() is expected to be called while in Auto
    // mode. Bulb level/color transitions are timed to this interval so the
    // gradient ramps continuously from the bulb's current state instead of
    // snapping and holding, i.e. "transition over the tick" rather than
    // "jump then wait". Defaults to 60s; the debug fast-day mode shortens
    // both this and the simulated day length, keeping the ramp smooth.
    void setTickInterval(std::chrono::seconds interval) noexcept;

    // The live schedule (active window, fade, work days). Changes apply from
    // the next update; caller must hold the Zigbee lock if the stack is up.
    [[nodiscard]] const DaylightSchedule::Config &scheduleConfig() const noexcept { return schedule_.config(); }
    void setScheduleConfig(const DaylightSchedule::Config &config) noexcept { schedule_.setConfig(config); }

    // Per-light peak intensity, 1-100 (% of full brightness): scales that
    // light's whole program (red alert excepted). Defaults: walls 100,
    // work 90. Caller must hold the Zigbee lock if the stack is up.
    [[nodiscard]] uint8_t peakPercent(Role role) const noexcept { return peaks_[static_cast<std::size_t>(role)]; }
    void setPeakPercent(Role role, uint8_t percent) noexcept;
    void setPeakPercents(const std::array<uint8_t, kRoleCount> &peaks) noexcept { peaks_ = peaks; }
    [[nodiscard]] const std::array<uint8_t, kRoleCount> &peakPercents() const noexcept { return peaks_; }

    // Smooth, continuous update for the periodic heartbeat tick: bulbs
    // ramp from their current level/color to the new target over the tick
    // interval set above.
    void applyRole(Role role);
    void applyAllBulbs();

    // Quick (2s) update for explicit state changes that should feel
    // immediate rather than mid-animation: resuming after an override, a
    // bulb rejoining, or a freshly-discovered bulb's first command.
    void snapRole(Role role);
    void snapAllBulbs();

    void forceAllOff();
    void driveRedAlertFlash();

    // Called from the ZDO signal handler on EZB_ZDO_SIGNAL_DEVICE_ANNCE.
    void handleDeviceAnnounce(uint64_t ext_addr, uint16_t short_addr);

    // Console-driven pairing (see pairing_console.h). The next device that
    // announces itself while not already holding a role is given the armed
    // role, taking precedence over the role manifest. Devices that already
    // hold a role are never claimed, so a known bulb power-cycling mid-pairing
    // can't be moved by accident - unpair it first to reassign it.
    void armPairing(Role role) noexcept;
    void cancelPairing() noexcept;
    [[nodiscard]] bool isAssigned(Role role) const noexcept { return roles_.at(role).valid; }

    // Re-sends endpoint discovery for every assigned device whose endpoint
    // is still unknown (e.g. its Match_Desc went unanswered right after
    // joining). Caller must hold the Zigbee lock.
    void retryMissingEndpoints();

    void unpair(Role role);
    void unpairAll();
    void logRoles() const;

    [[nodiscard]] std::optional<BulbEndpoint> bulbFor(Role role) const;

    // Liveness, for the status display: when each device was last heard
    // from (a command ack, a ping reply, an announce, a button press).
    struct DeviceStatus {
        bool    paired         = false;
        bool    has_endpoint   = false;
        int64_t last_heard_us  = 0;   // esp_timer time; 0 = not since boot
        int8_t  rssi           = 0;   // of the last ack, if any
    };
    [[nodiscard]] DeviceStatus deviceStatus(Role role) const noexcept;
    void noteHeardFrom(uint16_t short_addr, int8_t rssi) noexcept;
    void noteHeard(Role role) noexcept;

    // Pings every paired bulb (ZDO Active_EP request) so liveness stays
    // current even while no light commands are being sent (outside the
    // active window). Caller must hold the Zigbee lock.
    void pingDevices();

private:
    struct AppliedState {
        bool    valid = false;
        bool    on    = false;
        uint8_t level = 0;
    };

    void applyRoleWithTransition(Role role, BulbEndpoint::TransitionTime transition);
    [[nodiscard]] BulbEndpoint::TransitionTime tickTransition() const noexcept;
    [[nodiscard]] std::tm now() const;

    void discoverDevice(std::size_t role_index);
    void bindButton(std::size_t role_index);

    static void onMatchDescBulbResult(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx);
    static void onMatchDescButtonResult(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx);
    static void onBindButtonResult(const ezb_zdp_bind_req_result_t *result, void *user_ctx);
    static void onPingResult(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx);

    struct Liveness {
        int64_t last_heard_us = 0;
        int8_t  rssi          = 0;
    };

    RoleStore                   roles_;
    DaylightSchedule            schedule_;
    ClockFn                     clock_;
    UtcOffsetFn                 utc_offset_fn_;
    std::chrono::seconds        tick_interval_{60};
    std::array<AppliedState, 4> applied_{};
    bool                        red_alert_bright_ = false;
    std::optional<Role>         armed_role_;
    std::array<Liveness, kRoleCount> liveness_{};
    std::array<uint8_t, kRoleCount>  peaks_{100, 100, 100, 90, 100};
    bool                        require_synced_clock_ = true;
    bool                        clock_warned_         = false;

    static LightRig *s_self;
};

} // namespace sunsim
