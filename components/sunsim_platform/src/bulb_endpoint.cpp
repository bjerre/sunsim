#include "bulb_endpoint.h"

#include <cmath>

#include "ezbee/zha.h"

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

void BulbEndpoint::turnOn() const
{
    ezb_zcl_on_off_cmd_t cmd{.cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_)};
    ezb_zcl_on_off_on_cmd_req(&cmd);
}

void BulbEndpoint::turnOff() const
{
    ezb_zcl_on_off_cmd_t cmd{.cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_)};
    ezb_zcl_on_off_off_cmd_req(&cmd);
}

void BulbEndpoint::setLevel(uint8_t level_0_254, TransitionTime transition) const
{
    ezb_zcl_level_move_to_level_with_on_off_cmd_t cmd{
        .cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_), .payload = {}};
    cmd.payload.level           = level_0_254;
    cmd.payload.transition_time = transition.count();
    ezb_zcl_level_move_to_level_with_on_off_cmd_req(&cmd);
}

void BulbEndpoint::setColorTemperature(uint16_t mireds, TransitionTime transition) const
{
    ezb_zcl_color_control_move_to_color_temperature_cmd_t cmd{
        .cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_), .payload = {}};
    cmd.payload.color_temperature_mireds = mireds;
    cmd.payload.transition_time          = transition.count();
    ezb_zcl_color_control_move_to_color_temperature_cmd_req(&cmd);
}

void BulbEndpoint::setHueSaturation(uint8_t hue, uint8_t saturation, TransitionTime transition) const
{
    ezb_zcl_color_control_move_to_hue_and_saturation_cmd_t cmd{
        .cmd_ctrl = makeCmdCtrl(short_addr_, endpoint_, source_endpoint_), .payload = {}};
    cmd.payload.hue             = hue;
    cmd.payload.saturation      = saturation;
    cmd.payload.transition_time = transition.count();
    ezb_zcl_color_control_move_to_hue_and_saturation_cmd_req(&cmd);
}

void BulbEndpoint::setColorXY(double x, double y, TransitionTime transition) const
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
    ezb_zcl_color_control_move_to_color_cmd_req(&cmd);
}

} // namespace sunsim::platform
