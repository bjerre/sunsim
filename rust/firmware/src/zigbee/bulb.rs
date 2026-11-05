//! ZCL light commands, one function each.

use ev::Transition;

use super::ffi::*;

/// A group command's broadcast: to every device whose radio is always on
/// (the mains-powered ones, the bulbs among them), each keeping it if it's
/// in the group.
const RX_ON_WHEN_IDLE: u16 = 0xFFFD;

/// A Zigbee light endpoint identified by its short address, or a Zigbee
/// group's lights by its id, addressed directly (no binding table involved)
/// from sunsim's light-control endpoint. Each command returns the stack's
/// result for queuing it (not delivery): EZB_ERR_NONE when it was accepted.
/// None of them take the Zigbee lock: they're only called while the app task
/// processes events, which it does holding the lock.
#[derive(Clone, Copy, Debug)]
pub struct BulbEndpoint {
    address: u16,
    endpoint: u8,
    source_endpoint: u8,
    group: bool,
}

impl BulbEndpoint {
    pub fn light(short_addr: u16, endpoint: u8, source_endpoint: u8) -> Self {
        BulbEndpoint { address: short_addr, endpoint, source_endpoint, group: false }
    }

    /// Every light in the group: one broadcast, unacknowledged.
    pub fn group(group_id: u16, source_endpoint: u8) -> Self {
        BulbEndpoint { address: group_id, endpoint: 0, source_endpoint, group: true }
    }

    fn ctrl(&self) -> ezb_zcl_cluster_cmd_ctrl_t {
        // SAFETY: a C struct of plain fields; all-zero is its "{}".
        let mut ctrl: ezb_zcl_cluster_cmd_ctrl_t = unsafe { core::mem::zeroed() };
        if self.group {
            ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_GROUP;
            ctrl.dst_addr.u.group_addr = ezb_grpaddr_t { group: self.address, bcast: RX_ON_WHEN_IDLE };
        } else {
            ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
            ctrl.dst_addr.u.short_addr = self.address;
        }
        ctrl.dst_ep = self.endpoint;
        ctrl.src_ep = self.source_endpoint;
        ctrl
    }

    pub fn turn_on(&self) -> ezb_err_t {
        let cmd = ezb_zcl_on_off_cmd_t { cmd_ctrl: self.ctrl() };
        unsafe { ezb_zcl_on_off_on_cmd_req(&cmd) }
    }

    pub fn turn_off(&self) -> ezb_err_t {
        let cmd = ezb_zcl_on_off_cmd_t { cmd_ctrl: self.ctrl() };
        unsafe { ezb_zcl_on_off_off_cmd_req(&cmd) }
    }

    pub fn set_level(&self, level: u8, transition: Transition) -> ezb_err_t {
        // SAFETY: as in ctrl(); zero options mean none.
        let mut cmd: ezb_zcl_level_move_to_level_with_on_off_cmd_t = unsafe { core::mem::zeroed() };
        cmd.cmd_ctrl = self.ctrl();
        cmd.payload.level = level;
        cmd.payload.transition_time = transition.0;
        unsafe { ezb_zcl_level_move_to_level_with_on_off_cmd_req(&cmd) }
    }

    pub fn set_color_temperature(&self, mireds: u16, transition: Transition) -> ezb_err_t {
        let mut cmd: ezb_zcl_color_control_move_to_color_temperature_cmd_t = unsafe { core::mem::zeroed() };
        cmd.cmd_ctrl = self.ctrl();
        cmd.payload.color_temperature_mireds = mireds;
        cmd.payload.transition_time = transition.0;
        unsafe { ezb_zcl_color_control_move_to_color_temperature_cmd_req(&cmd) }
    }

    pub fn set_hue_saturation(&self, hue: u8, saturation: u8, transition: Transition) -> ezb_err_t {
        let mut cmd: ezb_zcl_color_control_move_to_hue_and_saturation_cmd_t = unsafe { core::mem::zeroed() };
        cmd.cmd_ctrl = self.ctrl();
        cmd.payload.hue = hue;
        cmd.payload.saturation = saturation;
        cmd.payload.transition_time = transition.0;
        unsafe { ezb_zcl_color_control_move_to_hue_and_saturation_cmd_req(&cmd) }
    }

    /// CIE 1931 xy chromaticity (each 0..1), for colors outside the bulb's
    /// color-temperature range (e.g. amber below ~2700K).
    pub fn set_color_xy(&self, x: f32, y: f32, transition: Transition) -> ezb_err_t {
        // ZCL encodes x and y as value * 65536, capped at 0xFEFF.
        let encode = |v: f32| (v as f64 * 65536.0).round().clamp(0.0, 0xFEFF as f64) as u16;
        let mut cmd: ezb_zcl_color_control_move_to_color_cmd_t = unsafe { core::mem::zeroed() };
        cmd.cmd_ctrl = self.ctrl();
        cmd.payload.color_x = encode(x);
        cmd.payload.color_y = encode(y);
        cmd.payload.transition_time = transition.0;
        unsafe { ezb_zcl_color_control_move_to_color_cmd_req(&cmd) }
    }
}
