#include "light_rig.h"

#include <chrono>
#include <ctime>
#include <limits>

#include "esp_log.h"
#include "esp_timer.h"

#include "role_manifest.h"
#include "sunsim_config.h"

namespace sunsim {

namespace {
constexpr const char *kTag = "LIGHT_RIG";

// Fixed, short transition for state changes that should feel immediate
// rather than mid-animation (see LightRig::snapRole).
constexpr BulbEndpoint::TransitionTime kSnapTransition{20}; // 2.0s

// Red alert pulses like TNG's red-alert lighting: a slow, continuous glow
// rather than a strobe. driveRedAlertFlash() alternates bright/dim once per
// 1s heartbeat (a 2s cycle, ~0.5 Hz), and each fade takes nearly the whole
// second, so the bulb is always gliding between the two.
constexpr BulbEndpoint::TransitionTime kRedAlertFade{9}; // 0.9s

std::tm localTimeNow() noexcept
{
    const auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm    now_tm{};
    localtime_r(&now_c, &now_tm);
    return now_tm;
}

// The current local civil UTC offset (including DST), derived without a
// portable tm_gmtoff: reinterpreting today's UTC calendar fields as if they
// were local time tells mktime() how far local time has drifted from UTC
// under the process's TZ rules.
double currentUtcOffsetHours() noexcept
{
    const auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm    utc_tm{};
    gmtime_r(&now_c, &utc_tm);
    const std::time_t utc_as_if_local = std::mktime(&utc_tm);
    return std::difftime(now_c, utc_as_if_local) / 3600.0;
}
} // namespace

LightRig *LightRig::s_self = nullptr;

LightRig::LightRig() : LightRig(DaylightSchedule{}) {}

LightRig::LightRig(DaylightSchedule schedule)
    : schedule_(schedule), clock_(&localTimeNow), utc_offset_fn_(&currentUtcOffsetHours)
{
    s_self = this;
}

void LightRig::load()
{
    roles_.load();
}

void LightRig::setClock(ClockFn clock)
{
    clock_ = std::move(clock);
}

void LightRig::setUtcOffsetFn(UtcOffsetFn utc_offset_fn)
{
    utc_offset_fn_ = std::move(utc_offset_fn);
}

void LightRig::setTickInterval(std::chrono::seconds interval) noexcept
{
    tick_interval_ = interval;
}

std::tm LightRig::now() const
{
    return clock_();
}

bool LightRig::clockValid() const
{
    // SNTP-synced time is always well past this; an unsynced clock starts at 1970.
    constexpr int kMinValidYear = 2024;
    return !require_synced_clock_ || now().tm_year + 1900 >= kMinValidYear;
}

LightRig::DeviceStatus LightRig::deviceStatus(Role role) const noexcept
{
    const DeviceRole &d = roles_.at(role);
    const Liveness   &l = liveness_[static_cast<std::size_t>(role)];
    return {.paired = d.valid, .has_endpoint = d.endpoint != 0, .last_heard_us = l.last_heard_us, .rssi = l.rssi};
}

void LightRig::noteHeardFrom(uint16_t short_addr, int8_t rssi) noexcept
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (roles_.at(i).valid && roles_.at(i).short_addr == short_addr) {
            liveness_[i] = {.last_heard_us = esp_timer_get_time(), .rssi = rssi};
        }
    }
}

void LightRig::noteHeard(Role role) noexcept
{
    liveness_[static_cast<std::size_t>(role)].last_heard_us = esp_timer_get_time();
}

void LightRig::pingDevices()
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const DeviceRole &d = roles_.at(i);
        // The button is a sleepy end device that won't answer; it counts as
        // heard when it's pressed instead.
        if (!d.valid || i == static_cast<std::size_t>(Role::Button)) {
            continue;
        }
        ezb_zdo_active_ep_req_t req{};
        req.dst_nwk_addr               = d.short_addr;
        req.field.nwk_addr_of_interest = d.short_addr;
        req.cb                         = &LightRig::onPingResult;
        req.user_ctx                   = reinterpret_cast<void *>(static_cast<intptr_t>(i));
        ezb_zdo_active_ep_req(&req);
    }
}

void LightRig::onPingResult(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    const auto idx = static_cast<std::size_t>(reinterpret_cast<intptr_t>(user_ctx));
    if (result && result->error == EZB_ERR_NONE && result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        s_self->liveness_[idx].last_heard_us = esp_timer_get_time();
    }
}

BulbEndpoint::TransitionTime LightRig::tickTransition() const noexcept
{
    constexpr auto kMaxDeciseconds = std::numeric_limits<uint16_t>::max();
    const auto     deciseconds     = tick_interval_.count() * 10;
    return BulbEndpoint::TransitionTime{
        static_cast<uint16_t>(deciseconds > kMaxDeciseconds ? kMaxDeciseconds : deciseconds)};
}

std::optional<BulbEndpoint> LightRig::bulbFor(Role role) const
{
    const DeviceRole &d = roles_.at(role);
    if (!d.valid || d.endpoint == 0) {
        return std::nullopt;
    }
    return BulbEndpoint{d.short_addr, d.endpoint, SUNSIM_LIGHT_CTRL_EP_ID};
}

void LightRig::applyRoleWithTransition(Role role, BulbEndpoint::TransitionTime transition)
{
    if (role == Role::Button) {
        return;
    }
    const auto bulb = bulbFor(role);
    if (!bulb) {
        return;
    }

    const std::size_t idx    = static_cast<std::size_t>(role);
    const std::tm     now_tm = now();
    LightState        desired{};
    if (clockValid()) {
        clock_warned_ = false;
        desired = (role == Role::Work) ? schedule_.workState(now_tm) : schedule_.wallState(now_tm, utc_offset_fn_());
    } else if (!clock_warned_) {
        clock_warned_ = true;
        ESP_LOGW(kTag, "Clock not set yet (no time sync) - holding lights off");
    }
    AppliedState &cache = applied_[idx];

    if (desired.on) {
        if (!cache.valid || !cache.on) {
            bulb->turnOn();
        }
        bulb->setLevel(desired.level, transition);
        bulb->setColorTemperature(desired.mireds, transition);
    } else if (!cache.valid || cache.on) {
        bulb->turnOff();
    }
    cache = {.valid = true, .on = desired.on, .level = desired.level};
}

void LightRig::applyRole(Role role)
{
    applyRoleWithTransition(role, tickTransition());
}

void LightRig::applyAllBulbs()
{
    applyRole(Role::WallA);
    applyRole(Role::WallB);
    applyRole(Role::WallC);
    applyRole(Role::Work);
}

void LightRig::snapRole(Role role)
{
    applyRoleWithTransition(role, kSnapTransition);
}

void LightRig::snapAllBulbs()
{
    snapRole(Role::WallA);
    snapRole(Role::WallB);
    snapRole(Role::WallC);
    snapRole(Role::Work);
}

void LightRig::forceAllOff()
{
    for (std::size_t idx = 0; idx < applied_.size(); ++idx) {
        const auto bulb = bulbFor(static_cast<Role>(idx));
        if (!bulb) {
            continue;
        }
        bulb->turnOff();
        applied_[idx] = {.valid = true, .on = false, .level = 0};
    }
}

void LightRig::driveRedAlertFlash()
{
    red_alert_bright_ = !red_alert_bright_;
    for (std::size_t idx = 0; idx < applied_.size(); ++idx) {
        const auto bulb = bulbFor(static_cast<Role>(idx));
        if (!bulb) {
            continue;
        }
        bulb->setHueSaturation(0, 254, BulbEndpoint::TransitionTime{2});
        bulb->setLevel(red_alert_bright_ ? 254 : 30, kRedAlertFade);
        applied_[idx].valid = false; // force a clean re-sync once we leave red alert
    }
}

void LightRig::handleDeviceAnnounce(uint64_t ext_addr, uint16_t short_addr)
{
    if (const auto existing = roles_.findByExtAddr(ext_addr)) {
        DeviceRole &d = roles_.at(*existing);
        if (d.short_addr != short_addr) {
            d.short_addr = short_addr;
            roles_.save();
        }
        ESP_LOGI(kTag, "Known device rejoined: role %u short=0x%04x", static_cast<unsigned>(*existing), short_addr);
        noteHeard(static_cast<Role>(*existing));
        if (*existing == static_cast<std::size_t>(Role::Button) || d.endpoint == 0) {
            discoverDevice(*existing);
        } else {
            snapRole(static_cast<Role>(*existing));
        }
        return;
    }

    if (armed_role_) {
        const auto role_index = static_cast<std::size_t>(*armed_role_);
        armed_role_.reset();
        roles_.at(role_index) = {.valid = true, .ext_addr = ext_addr, .short_addr = short_addr, .endpoint = 0};
        if (role_index < applied_.size()) {
            applied_[role_index] = {};
        }
        roles_.save();
        noteHeard(static_cast<Role>(role_index));
        ESP_LOGI(kTag, "Paired ext=0x%016llx short=0x%04x -> %s", static_cast<unsigned long long>(ext_addr),
                 short_addr, roleName(role_index).data());
        discoverDevice(role_index);
        return;
    }

    const auto role = roleForExtAddr(ext_addr);
    if (!role) {
        ESP_LOGW(kTag, "Ignoring unknown device 0x%04x (ext=0x%016llx): not in role manifest (use `pair <role>` to claim it)", short_addr,
                 static_cast<unsigned long long>(ext_addr));
        return;
    }

    const auto role_index = static_cast<std::size_t>(*role);
    if (roles_.at(role_index).valid) {
        ESP_LOGW(kTag, "Ignoring device 0x%04x: role %u already assigned to a different device", short_addr,
                 static_cast<unsigned>(role_index));
        return;
    }

    roles_.at(role_index) = {.valid = true, .ext_addr = ext_addr, .short_addr = short_addr, .endpoint = 0};
    roles_.save();
    noteHeard(*role);

    ESP_LOGI(kTag, "New device joined -> role %u (%s)", static_cast<unsigned>(role_index),
             role_index == static_cast<std::size_t>(Role::Button) ? "button" : "bulb");
    discoverDevice(role_index);
}

void LightRig::retryMissingEndpoints()
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const DeviceRole &d = roles_.at(i);
        if (d.valid && d.endpoint == 0) {
            ESP_LOGI(kTag, "Retrying endpoint discovery for %s (short=0x%04x)", roleName(i).data(), d.short_addr);
            discoverDevice(i);
        }
    }
}

void LightRig::armPairing(Role role) noexcept
{
    armed_role_ = role;
}

void LightRig::cancelPairing() noexcept
{
    armed_role_.reset();
}

void LightRig::unpair(Role role)
{
    const auto idx = static_cast<std::size_t>(role);
    roles_.clear(idx);
    if (idx < applied_.size()) {
        applied_[idx] = {};
    }
    roles_.save();
}

void LightRig::unpairAll()
{
    roles_.clearAll();
    applied_ = {};
    roles_.save();
}

void LightRig::logRoles() const
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const DeviceRole &d = roles_.at(i);
        if (d.valid) {
            ESP_LOGI(kTag, "%-6s ext=0x%016llx short=0x%04x ep=%d", roleName(i).data(),
                     static_cast<unsigned long long>(d.ext_addr), d.short_addr, d.endpoint);
        } else {
            ESP_LOGI(kTag, "%-6s (unassigned)", roleName(i).data());
        }
    }
    if (armed_role_) {
        ESP_LOGI(kTag, "Armed: next joining device becomes %s",
                 roleName(static_cast<std::size_t>(*armed_role_)).data());
    }
}

void LightRig::discoverDevice(std::size_t role_index)
{
    DeviceRole &d   = roles_.at(role_index);
    void       *ctx = reinterpret_cast<void *>(static_cast<intptr_t>(role_index));

    // No Zigbee lock here: discoverDevice() only runs from within Zigbee
    // callbacks, where the stack already holds the lock (see
    // esp_zigbee_lock_acquire's doc comment), or from retryMissingEndpoints(),
    // whose caller holds it.
    if (role_index == static_cast<std::size_t>(Role::Button)) {
        static uint16_t           out_clusters[] = {EZB_ZCL_CLUSTER_ID_ON_OFF};
        ezb_zdo_match_desc_req_t req{};
        req.dst_nwk_addr               = d.short_addr;
        req.field.nwk_addr_of_interest = d.short_addr;
        req.field.profile_id           = EZB_AF_HA_PROFILE_ID;
        req.field.num_in_clusters      = 0;
        req.field.num_out_clusters     = 1;
        req.field.cluster_list         = out_clusters;
        req.cb                         = &LightRig::onMatchDescButtonResult;
        req.user_ctx                   = ctx;
        ezb_zdo_match_desc_req(&req);
    } else {
        static uint16_t in_clusters[] = {EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_ID_LEVEL,
                                          EZB_ZCL_CLUSTER_ID_COLOR_CONTROL};
        ezb_zdo_match_desc_req_t req{};
        req.dst_nwk_addr               = d.short_addr;
        req.field.nwk_addr_of_interest = d.short_addr;
        req.field.profile_id           = EZB_AF_HA_PROFILE_ID;
        req.field.num_in_clusters      = 3;
        req.field.num_out_clusters     = 0;
        req.field.cluster_list         = in_clusters;
        req.cb                         = &LightRig::onMatchDescBulbResult;
        req.user_ctx                   = ctx;
        ezb_zdo_match_desc_req(&req);
    }
}

void LightRig::bindButton(std::size_t role_index)
{
    // No Zigbee lock here either: only ever called from onMatchDescButtonResult,
    // itself a Zigbee (ZDO result) callback.
    DeviceRole &d = roles_.at(role_index);

    ezb_extaddr_t self_ext;
    ezb_get_extended_address(&self_ext);
    ezb_extaddr_t button_ext{};
    button_ext.u64 = d.ext_addr;

    ezb_zdo_bind_req_t bind_req{};
    bind_req.dst_nwk_addr                 = d.short_addr;
    bind_req.field.src_addr               = button_ext;
    bind_req.field.src_ep                 = d.endpoint;
    bind_req.field.cluster_id             = EZB_ZCL_CLUSTER_ID_ON_OFF;
    bind_req.field.dst_addr_mode          = EZB_ADDR_MODE_EXT;
    bind_req.field.dst_addr.extended_addr = self_ext;
    bind_req.field.dst_ep                 = SUNSIM_BUTTON_RX_EP_ID;
    bind_req.cb                           = &LightRig::onBindButtonResult;
    bind_req.user_ctx                     = reinterpret_cast<void *>(static_cast<intptr_t>(role_index));

    ezb_zdo_bind_req(&bind_req);
}

void LightRig::onMatchDescBulbResult(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    const auto idx = static_cast<std::size_t>(reinterpret_cast<intptr_t>(user_ctx));
    if (!result || result->error != EZB_ERR_NONE || !result->rsp ||
        result->rsp->status != EZB_ZDP_STATUS_SUCCESS || result->rsp->match_length == 0) {
        ESP_LOGW(kTag, "Could not find light endpoint for %s (err=0x%x status=0x%x matches=%d), will retry",
                 roleName(idx).data(), result ? result->error : -1,
                 (result && result->rsp) ? result->rsp->status : -1,
                 (result && result->rsp) ? result->rsp->match_length : -1);
        return;
    }
    DeviceRole &d = s_self->roles_.at(idx);
    d.endpoint    = result->rsp->match_list[0];
    s_self->roles_.save();
    ESP_LOGI(kTag, "Role %u (bulb) endpoint = %d", static_cast<unsigned>(idx), d.endpoint);
    s_self->snapRole(static_cast<Role>(idx));
}

void LightRig::onMatchDescButtonResult(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    const auto idx = static_cast<std::size_t>(reinterpret_cast<intptr_t>(user_ctx));
    if (!result || result->error != EZB_ERR_NONE || !result->rsp ||
        result->rsp->status != EZB_ZDP_STATUS_SUCCESS || result->rsp->match_length == 0) {
        ESP_LOGW(kTag, "Could not find On/Off output endpoint on button (role %u)", static_cast<unsigned>(idx));
        return;
    }
    DeviceRole &d = s_self->roles_.at(idx);
    d.endpoint    = result->rsp->match_list[0];
    s_self->roles_.save();
    ESP_LOGI(kTag, "Button endpoint = %d, binding to local receiver", d.endpoint);
    s_self->bindButton(idx);
}

void LightRig::onBindButtonResult(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    const auto idx = static_cast<std::size_t>(reinterpret_cast<intptr_t>(user_ctx));
    if (result && result->error == EZB_ERR_NONE && result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(kTag, "Bound button (role %u) to local receiver endpoint", static_cast<unsigned>(idx));
    } else {
        ESP_LOGE(kTag, "Failed to bind button (role %u)", static_cast<unsigned>(idx));
    }
}

} // namespace sunsim
