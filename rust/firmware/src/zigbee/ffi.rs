//! esp-zigbee-lib's C names, and the few raw-pointer reads the adapter
//! needs: the only place stack memory is looked at.
//!
//! bindgen names each enum constant after its type
//! (`ezb_addr_mode_e_EZB_ADDR_MODE_SHORT`); these give them back their C
//! names, typed as the APIs take them. They matter in a `match` too: a bare
//! name that isn't a constant there is a binding that matches everything.

pub use esp_idf_svc::sys::zigbee::*;

macro_rules! c_names {
    ($($name:ident: $ty:ty = $bound:ident;)*) => {
        $(pub const $name: $ty = $bound as $ty;)*
    };
}

c_names! {
    EZB_NWK_DEVICE_TYPE_COORDINATOR: ezb_nwk_device_type_t = ezb_nwk_device_type_t_EZB_NWK_DEVICE_TYPE_COORDINATOR;
    ESP_ZIGBEE_RADIO_MODE_NATIVE: esp_zigbee_radio_mode_t = esp_zigbee_radio_mode_t_ESP_ZIGBEE_RADIO_MODE_NATIVE;

    EZB_ZDO_SIGNAL_SKIP_STARTUP: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_ZDO_SIGNAL_SKIP_STARTUP;
    EZB_ZDO_SIGNAL_DEVICE_ANNCE: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_ZDO_SIGNAL_DEVICE_ANNCE;
    EZB_ZDO_SIGNAL_DEVICE_UPDATE: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_ZDO_SIGNAL_DEVICE_UPDATE;
    EZB_ZDO_SIGNAL_LEAVE: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_ZDO_SIGNAL_LEAVE;
    EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS;
    EZB_BDB_SIGNAL_DEVICE_FIRST_START: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_BDB_SIGNAL_DEVICE_FIRST_START;
    EZB_BDB_SIGNAL_DEVICE_REBOOT: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_BDB_SIGNAL_DEVICE_REBOOT;
    EZB_BDB_SIGNAL_FORMATION: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_BDB_SIGNAL_FORMATION;
    EZB_BDB_SIGNAL_STEERING: ezb_app_signal_type_t = ezb_app_signal_type_e_EZB_BDB_SIGNAL_STEERING;

    EZB_BDB_STATUS_SUCCESS: ezb_bdb_comm_status_t = ezb_bdb_comm_status_e_EZB_BDB_STATUS_SUCCESS;
    EZB_BDB_MODE_INITIALIZATION: ezb_bdb_comm_mode_mask_t = ezb_bdb_comm_mode_e_EZB_BDB_MODE_INITIALIZATION;
    EZB_BDB_MODE_NETWORK_FORMATION: ezb_bdb_comm_mode_mask_t = ezb_bdb_comm_mode_e_EZB_BDB_MODE_NETWORK_FORMATION;
    EZB_BDB_MODE_NETWORK_STEERING: ezb_bdb_comm_mode_mask_t = ezb_bdb_comm_mode_e_EZB_BDB_MODE_NETWORK_STEERING;

    EZB_ERR_NONE: ezb_err_t = EZB_ERR_NONE_U32;
    EZB_ZDP_STATUS_SUCCESS: ezb_zdp_status_t = ezb_zdp_status_e_EZB_ZDP_STATUS_SUCCESS;
    EZB_ADDR_MODE_SHORT: ezb_addr_mode_t = ezb_addr_mode_e_EZB_ADDR_MODE_SHORT;
    EZB_ADDR_MODE_EXT: ezb_addr_mode_t = ezb_addr_mode_e_EZB_ADDR_MODE_EXT;
    EZB_ADDR_MODE_GROUP: ezb_addr_mode_t = ezb_addr_mode_e_EZB_ADDR_MODE_GROUP;
    EZB_AF_HA_PROFILE_ID: u16 = ezb_af_profile_id_e_EZB_AF_HA_PROFILE_ID;

    EZB_ZCL_CLUSTER_ID_BASIC: u16 = ezb_zcl_cluster_id_e_EZB_ZCL_CLUSTER_ID_BASIC;
    EZB_ZCL_CLUSTER_ID_ON_OFF: u16 = ezb_zcl_cluster_id_e_EZB_ZCL_CLUSTER_ID_ON_OFF;
    EZB_ZCL_CLUSTER_ID_LEVEL: u16 = ezb_zcl_cluster_id_e_EZB_ZCL_CLUSTER_ID_LEVEL;
    EZB_ZCL_CLUSTER_ID_COLOR_CONTROL: u16 = ezb_zcl_cluster_id_e_EZB_ZCL_CLUSTER_ID_COLOR_CONTROL;
    EZB_ZCL_CLUSTER_SERVER: u8 = EZB_ZCL_CLUSTER_SERVER_U32;
    EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID: u16 = ezb_zcl_basic_server_attr_t_EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID;
    EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID: u16 = ezb_zcl_basic_server_attr_t_EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID;

    EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: ezb_zcl_core_action_callback_id_t = ezb_zcl_core_action_callback_id_e_EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID;
    EZB_ZCL_CORE_SCENES_RECALL_SCENE_CB_ID: ezb_zcl_core_action_callback_id_t = ezb_zcl_core_action_callback_id_e_EZB_ZCL_CORE_SCENES_RECALL_SCENE_CB_ID;
    EZB_ZCL_CORE_ON_OFF_OFF_WITH_EFFECT_CB_ID: ezb_zcl_core_action_callback_id_t = ezb_zcl_core_action_callback_id_e_EZB_ZCL_CORE_ON_OFF_OFF_WITH_EFFECT_CB_ID;
    EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: ezb_zcl_core_action_callback_id_t = ezb_zcl_core_action_callback_id_e_EZB_ZCL_CORE_DEFAULT_RSP_CB_ID;
}

// The two that bindgen gives as plain macros, not enum members.
use esp_idf_svc::sys::zigbee::{EZB_ERR_NONE as EZB_ERR_NONE_U32, EZB_ZCL_CLUSTER_SERVER as EZB_ZCL_CLUSTER_SERVER_U32};

/// A signal's parameters, as the type that signal carries.
///
/// # Safety
/// `signal` is the stack's, for the duration of its handler, and carries a T.
pub unsafe fn params<'a, T>(signal: *const ezb_app_signal_t) -> &'a T {
    unsafe { &*(ezb_app_signal_get_params(signal) as *const T) }
}

/// A signal type's name, for the log.
pub fn signal_name(kind: ezb_app_signal_type_t) -> String {
    // SAFETY: the stack's names are static, NUL-terminated strings.
    unsafe { std::ffi::CStr::from_ptr(ezb_app_signal_to_string(kind)) }.to_string_lossy().into_owned()
}

/// A ZDO result's response, if the request got through and has one.
///
/// # Safety
/// `rsp` is null or the stack's, for the duration of the callback.
pub unsafe fn response<'a, T>(error: ezb_err_t, rsp: *const T) -> Option<&'a T> {
    if error != EZB_ERR_NONE { None } else { unsafe { rsp.as_ref() } }
}

/// An extended (IEEE) address as a number.
pub fn ext_of(addr: &ezb_extaddr_t) -> u64 {
    // SAFETY: both views of the union are 8 plain bytes.
    unsafe { addr.__bindgen_anon_1.u64_ }
}

pub fn ext_addr(value: u64) -> ezb_extaddr_t {
    ezb_extaddr_t { __bindgen_anon_1: ezb_eui64_s__bindgen_ty_1 { u64_: value } }
}

/// The short address an address holds, if it's in short mode.
#[allow(clippy::unnecessary_lazy_evaluations)] // only read the member the mode says is set
pub fn short_of(addr: &ezb_address_t) -> Option<u16> {
    // SAFETY: the mode says which member is set.
    (addr.addr_mode == EZB_ADDR_MODE_SHORT).then(|| unsafe { addr.u.short_addr })
}
