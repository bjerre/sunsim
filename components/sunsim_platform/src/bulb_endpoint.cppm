module;

#include "ezbee/core_types.h"
#include "ezbee/zha.h"

module sunsim.platform:bulb_endpoint; // internal: not exported

import std.compat;
import sunsim.ev;

namespace sunsim::platform {

// A Zigbee light endpoint identified by its short address, addressed
// directly (no binding table involved) from sunsim's light-control
// endpoint. Cheap, copyable value type - a BulbEndpoint is just an address.
// Each command returns the stack's result for queuing it (not delivery);
// EZB_ERR_NONE when it was accepted for sending.
class BulbEndpoint {
public:
    constexpr BulbEndpoint(uint16_t short_addr, uint8_t endpoint, uint8_t source_endpoint) noexcept
        : short_addr_(short_addr), endpoint_(endpoint), source_endpoint_(source_endpoint)
    {
    }

    ezb_err_t turnOn() const;
    ezb_err_t turnOff() const;
    ezb_err_t setLevel(uint8_t level_0_254, ev::Transition transition = ev::Transition{1}) const;
    ezb_err_t setColorTemperature(uint16_t mireds, ev::Transition transition = ev::Transition{1}) const;
    ezb_err_t setHueSaturation(uint8_t hue, uint8_t saturation, ev::Transition transition = ev::Transition{1}) const;
    // CIE 1931 xy chromaticity (each 0..1), for colours outside the bulb's
    // colour-temperature range (e.g. amber below ~2700K).
    ezb_err_t setColorXY(double x, double y, ev::Transition transition = ev::Transition{1}) const;

    [[nodiscard]] constexpr uint16_t shortAddress() const noexcept { return short_addr_; }
    [[nodiscard]] constexpr uint8_t endpoint() const noexcept { return endpoint_; }

private:
    uint16_t short_addr_;
    uint8_t  endpoint_;
    uint8_t  source_endpoint_;
};

} // namespace sunsim::platform

namespace sunsim::platform {

namespace {

ezb_zcl_cluster_cmd_ctrl_t makeCmdCtrl(uint16_t short_addr, uint8_t dst_ep, uint8_t src_ep)
{
    ezb_zcl_cluster_cmd_ctrl_t ctrl{};
    ctrl.dst_addr.addr_mode    = EZB_ADDR_MODE_SHORT;
    ctrl.dst_addr.u.short_addr = short_addr;
    ctrl.dst_ep                = dst_ep;
    ctrl.src_ep                = src_ep;
    return ctrl;
}

} // namespace

// None of these take the Zigbee lock: they're only called by ZigbeeAdapter
// while the app task processes events, which it does holding the lock.

ezb_err_t BulbEndpoint::turnOn() const
{
    ezb_zcl_on_off_cmd_t cmd{.cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_)};
    return ezb_zcl_on_off_on_cmd_req(&cmd);
}

ezb_err_t BulbEndpoint::turnOff() const
{
    ezb_zcl_on_off_cmd_t cmd{.cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_)};
    return ezb_zcl_on_off_off_cmd_req(&cmd);
}

ezb_err_t BulbEndpoint::setLevel(uint8_t level_0_254, ev::Transition transition) const
{
    ezb_zcl_level_move_to_level_with_on_off_cmd_t cmd{
        .cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_), .payload = {}};
    cmd.payload.level           = level_0_254;
    cmd.payload.transition_time = transition.count();
    return ezb_zcl_level_move_to_level_with_on_off_cmd_req(&cmd);
}

ezb_err_t BulbEndpoint::setColorTemperature(uint16_t mireds, ev::Transition transition) const
{
    ezb_zcl_color_control_move_to_color_temperature_cmd_t cmd{
        .cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_), .payload = {}};
    cmd.payload.color_temperature_mireds = mireds;
    cmd.payload.transition_time          = transition.count();
    return ezb_zcl_color_control_move_to_color_temperature_cmd_req(&cmd);
}

ezb_err_t BulbEndpoint::setHueSaturation(uint8_t hue, uint8_t saturation, ev::Transition transition) const
{
    ezb_zcl_color_control_move_to_hue_and_saturation_cmd_t cmd{
        .cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_), .payload = {}};
    cmd.payload.hue             = hue;
    cmd.payload.saturation      = saturation;
    cmd.payload.transition_time = transition.count();
    return ezb_zcl_color_control_move_to_hue_and_saturation_cmd_req(&cmd);
}

ezb_err_t BulbEndpoint::setColorXY(double x, double y, ev::Transition transition) const
{
    // ZCL encodes x and y as value * 65536, capped at 0xFEFF.
    const auto encode = [](double v) {
        const long scaled = std::lround(v * 65536.0);
        return static_cast<uint16_t>(scaled < 0 ? 0 : (scaled > 0xFEFF ? 0xFEFF : scaled));
    };
    ezb_zcl_color_control_move_to_color_cmd_t cmd{
        .cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_), .payload = {}};
    cmd.payload.color_x         = encode(x);
    cmd.payload.color_y         = encode(y);
    cmd.payload.transition_time = transition.count();
    return ezb_zcl_color_control_move_to_color_cmd_req(&cmd);
}

} // namespace sunsim::platform
