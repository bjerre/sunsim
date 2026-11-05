//! The only code that talks to esp-zigbee-lib. Runs the stack (its own
//! task), forms/rejoins sunsim's network, and translates in both directions:
//!
//! ```text
//! stack -> events : NetworkUp, DeviceAnnounced, EndpointFound, BindResult,
//!                   PingResult, AckReceived, ButtonPressed
//! events -> stack : SendLight, Discover, Bind, Ping, OpenNetwork, FactoryReset
//! ```
//!
//! and paces the bulb commands: while the core's SendQueue has some waiting
//! (SendsWaiting), it posts a SendSlot every SEND_GAP for the next one. The
//! handlers run on the app task, which holds the stack's lock (StackLock)
//! while processing events; the stack's callbacks, on its task, only post.
//! A port of the C++ platform::ZigbeeAdapter.

mod bulb;
mod ffi;

use std::ffi::c_void;
use std::sync::atomic::{AtomicU8, AtomicU16, Ordering};
use std::sync::{Condvar, Mutex, OnceLock};

use esp_idf_svc::sys::{EspError, esp};
use esp_idf_svc::timer::{EspTaskTimerService, EspTimer};
use ev::*;

use crate::clock;
use bulb::BulbEndpoint;
use ffi::*;

const TAG: &str = "ZIGBEE";

const SEND_REPORT: Duration = Duration::secs(10);

/// Between bulb commands: what the stack's buffers keep up with, and long
/// enough that what a moment's events ask of every light (the program's
/// targets and the weather's, a bus pass or two apart) is all waiting by the
/// first slot, so the lights showing the same can share a group command.
const SEND_GAP: std::time::Duration = std::time::Duration::from_millis(25);

/// Endpoint on which sunsim issues On/Off, Level and Color Control client
/// commands to the bulbs.
const LIGHT_CTRL_EP: u8 = 1;

/// Endpoint on which sunsim hosts an On/Off (+ Scenes) server, bound to the
/// physical button controller so its presses arrive here.
const BUTTON_RX_EP: u8 = 2;

/// The button: there's one at most (LAYOUT checks), and it's the one whose
/// presses arrive. (With none, none arrive: any role will do.)
const BUTTON: Role = {
    let mut i = 0;
    while i < ROLE_COUNT && !is_button(ROLES[i]) {
        i += 1;
    }
    if i < ROLE_COUNT { ROLES[i] } else { ROLES[0] }
};

// The C++ firmware's Kconfig defaults (main/Kconfig.projbuild).
const PRIMARY_CHANNEL_MASK: u32 = 1 << 13;
const SECONDARY_CHANNEL_MASK: u32 = 0x07FF_F800;

// ZCL character strings: a length byte, then the characters (no NUL).
static MANUFACTURER_NAME: [u8; 7] = *b"\x06sunsim";
static MODEL_IDENTIFIER: [u8; 8] = *b"\x07esp32c6";

const NO_BUTTON: u16 = 0xFFFF;

/// The ZCL frame header's fields that pick out the button's commands (ZCL
/// spec 2.4.1): the frame control byte, then (if manufacturer-specific) a
/// 2-byte manufacturer code, the sequence number, and the command.
mod zcl {
    pub const FRAME_TYPE_MASK: u8 = 0x03; // frame control bits 0-1
    pub const CLUSTER_SPECIFIC: u8 = 0x01; // ... a cluster's own command, not a global one
    pub const MANUFACTURER_SPECIFIC: u8 = 0x04; // frame control bit 2
    pub const DEFAULT_RESPONSE: u8 = 0x0B; // the global command

    // On/Off cluster commands.
    pub const OFF: u8 = 0x00;
    pub const ON: u8 = 0x01;
    pub const TOGGLE: u8 = 0x02;
}

// A role through a ZDO request's user_ctx, as its slot.
fn role_ctx(role: Role) -> *mut c_void {
    std::ptr::without_provenance_mut(slot(role))
}

fn role_from_ctx(ctx: *mut c_void) -> Role {
    ROLES[ctx.addr()]
}

/// The stack's API lock, held by the app task around event processing:
/// released when dropped.
pub struct StackLock(());

impl StackLock {
    pub fn acquire() -> Self {
        // SAFETY: the lock exists once the stack is initialized (wait_until_ready).
        unsafe { esp_zigbee_lock_acquire(u32::MAX) }; // portMAX_DELAY
        StackLock(())
    }
}

impl Drop for StackLock {
    fn drop(&mut self) {
        unsafe { esp_zigbee_lock_release() };
    }
}

// What the stack's callbacks reach: its signal, ZCL and APS handlers take no
// context pointer (its ZDO requests do: user_ctx carries the role).
struct Callbacks {
    post: Post,
    // The button's short address (NO_BUTTON = none), read by the APS hook on
    // the Zigbee task to decide synchronously whether to consume a frame.
    button_short: AtomicU16,
    ready: (Mutex<bool>, Condvar),
    timers: EspTaskTimerService,
    retry: Mutex<Option<EspTimer<'static>>>,
    retry_mode: AtomicU8,
}

static CALLBACKS: OnceLock<Callbacks> = OnceLock::new();

fn callbacks() -> &'static Callbacks {
    CALLBACKS.get().expect("the adapter exists before the stack calls back")
}

fn post(e: impl Into<Event>) {
    callbacks().post.send(e);
}

// Bulb commands the stack refused to queue (e.g. out of buffers), summarised
// every SEND_REPORT so silent drops show up in the log, with the most lights
// that had commands waiting meanwhile.
#[derive(Default)]
struct SendStats {
    sends: u32,
    failures: u32,
    group_sends: u32,
    backlog_peak: u8,
    last_error: ezb_err_t,
    report_start: Instant,
}

/// The board's radio: ev::Radio for esp-zigbee-lib. One per program
/// (the stack's callbacks find it through a static).
pub struct ZigbeeAdapter {
    send_timer: EspTimer<'static>, // the next SendSlot
    stats: SendStats,
}

impl ZigbeeAdapter {
    pub fn new(post: Post) -> Result<Self, EspError> {
        let timers = EspTaskTimerService::new()?;
        let slot_post = post.clone();
        let send_timer = timers.timer(move || slot_post.send(SendSlot { at: clock::now() }))?;
        let made = CALLBACKS.set(Callbacks {
            post,
            button_short: AtomicU16::new(NO_BUTTON),
            ready: (Mutex::new(false), Condvar::new()),
            timers,
            retry: Mutex::new(None),
            retry_mode: AtomicU8::new(0),
        });
        assert!(made.is_ok(), "one ZigbeeAdapter");
        Ok(ZigbeeAdapter { send_timer, stats: SendStats::default() })
    }

    /// Spawns the Zigbee task.
    pub fn start(&self) {
        log::info!(target: TAG, "Starting Zigbee stack");
        std::thread::Builder::new()
            .name("Zigbee_main".into())
            // 6 KB: the stack's callbacks run here and post events (~256 B per copy).
            .stack_size(6144)
            .spawn(|| {
                if let Err(e) = run_stack() {
                    log::error!(target: TAG, "Zigbee stack: {e}");
                }
            })
            .expect("the Zigbee task starts");
    }

    /// Blocks until the stack (and with it the Zigbee lock) exists.
    pub fn wait_until_ready(&self) {
        let (ready, cv) = &callbacks().ready;
        let _ready = cv.wait_while(ready.lock().unwrap(), |r| !*r).unwrap();
    }

    fn count_send(&mut self, err: ezb_err_t) {
        let s = &mut self.stats;
        s.sends += 1;
        if err != EZB_ERR_NONE {
            s.failures += 1;
            s.last_error = err;
        }
        let now = clock::now();
        if now - s.report_start >= SEND_REPORT {
            let secs = (now - s.report_start).floor_secs();
            if s.failures > 0 {
                log::warn!(
                    target: TAG,
                    "{} of {} bulb commands not queued in the last {secs}s (last err 0x{:x}; up to {} lights waiting)",
                    s.failures,
                    s.sends,
                    s.last_error,
                    s.backlog_peak
                );
            } else {
                log::debug!(
                    target: TAG,
                    "{} bulb commands sent in the last {secs}s, {} to groups; up to {} lights waiting",
                    s.sends,
                    s.group_sends,
                    s.backlog_peak
                );
            }
            *s = SendStats { report_start: now, ..SendStats::default() };
        }
    }
}

impl Radio for ZigbeeAdapter {
    fn sends_waiting(&mut self, _: &SendsWaiting) {
        if !self.send_timer.is_scheduled().unwrap_or(false) {
            let _ = self.send_timer.after(SEND_GAP);
        }
    }

    fn send_light(&mut self, e: &SendLight) {
        let bulb = if e.to.group {
            BulbEndpoint::group(e.to.address, LIGHT_CTRL_EP)
        } else {
            BulbEndpoint::light(e.to.address, e.to.endpoint, LIGHT_CTRL_EP)
        };
        self.stats.backlog_peak = self.stats.backlog_peak.max(e.backlog);
        self.stats.group_sends += e.to.group as u32;
        let err = match (e.power, e.level, e.color) {
            (Some(true), ..) => bulb.turn_on(),
            (Some(false), ..) => bulb.turn_off(),
            (None, Some(level), _) => bulb.set_level(level, e.transition),
            (None, None, Some(Color::HueSat(c))) => bulb.set_hue_saturation(c.hue, c.saturation, e.transition),
            (None, None, Some(Color::Xy(c))) => bulb.set_color_xy(c.x, c.y, e.transition),
            (None, None, Some(Color::Mireds(c))) => bulb.set_color_temperature(c.0, e.transition),
            (None, None, None) => return,
        };
        self.count_send(err);
    }

    fn discover(&mut self, e: &Discover) {
        // Its endpoint with any of these: a light's server clusters (what it
        // takes), a button's On/Off client (what it sends). The stack only
        // reads them.
        static COLOR_BULB: [u16; 3] = [EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL];
        static ON_OFF: [u16; 1] = [EZB_ZCL_CLUSTER_ID_ON_OFF];
        // SAFETY: a C struct of plain fields and pointers; all-zero is its "{}".
        let mut req: ezb_zdo_match_desc_req_t = unsafe { core::mem::zeroed() };
        req.dst_nwk_addr = e.short_addr;
        req.field.nwk_addr_of_interest = e.short_addr;
        req.field.profile_id = EZB_AF_HA_PROFILE_ID;
        req.cb = Some(on_match_desc_result);
        req.user_ctx = role_ctx(e.role);
        let (clusters, input) = match kind_of(e.role) {
            Kind::ColorBulb => (&COLOR_BULB[..], true),
            Kind::Plug => (&ON_OFF[..], true),
            Kind::Button => (&ON_OFF[..], false),
        };
        if input {
            req.field.num_in_clusters = clusters.len() as u8;
        } else {
            req.field.num_out_clusters = clusters.len() as u8;
        }
        req.field.cluster_list = clusters.as_ptr().cast_mut();
        unsafe { ezb_zdo_match_desc_req(&req) };
    }

    fn bind(&mut self, e: &Bind) {
        let mut self_ext = ext_addr(0);
        // SAFETY: as in discover(); the stack fills in our address.
        let mut req: ezb_zdo_bind_req_t = unsafe {
            ezb_get_extended_address(&mut self_ext);
            core::mem::zeroed()
        };
        req.dst_nwk_addr = e.short_addr;
        req.field.src_addr = ext_addr(e.ext_addr);
        req.field.src_ep = e.endpoint;
        req.field.cluster_id = EZB_ZCL_CLUSTER_ID_ON_OFF;
        req.field.dst_addr_mode = EZB_ADDR_MODE_EXT;
        req.field.dst_addr.extended_addr = self_ext;
        req.field.dst_ep = BUTTON_RX_EP;
        req.cb = Some(on_bind_result);
        req.user_ctx = role_ctx(e.role);
        unsafe { ezb_zdo_bind_req(&req) };
    }

    fn ping(&mut self, e: &Ping) {
        let mut req: ezb_zdo_active_ep_req_t = unsafe { core::mem::zeroed() };
        req.dst_nwk_addr = e.short_addr;
        req.field.nwk_addr_of_interest = e.short_addr;
        req.cb = Some(on_ping_result);
        req.user_ctx = role_ctx(e.role);
        unsafe { ezb_zdo_active_ep_req(&req) };
    }

    fn open_network(&mut self, e: &OpenNetwork) {
        let err = unsafe { ezb_bdb_open_network(e.secs) };
        if err != EZB_ERR_NONE {
            log::warn!(target: TAG, "Could not open network (err 0x{err:x}) - has it finished forming?");
        }
    }

    fn factory_reset(&mut self, _: &FactoryReset) {
        log::warn!(target: TAG, "Clearing the Zigbee network, then restarting");
        unsafe { esp_zigbee_factory_reset() } // does not return
    }
}

impl Module for ZigbeeAdapter {
    fn on(&mut self, e: &Event) {
        if requests(self, e) {
            return;
        }
        let button = match e {
            Event::RoleAssigned(e) if is_button(e.role) => {
                if e.record.valid { e.record.short_addr } else { NO_BUTTON }
            }
            Event::RoleCleared(e) if is_button(e.role) => NO_BUTTON,
            _ => return,
        };
        callbacks().button_short.store(button, Ordering::Relaxed);
    }
}

// ---- Stack lifecycle (Zigbee task) ---------------------------------------------

fn run_stack() -> Result<(), EspError> {
    // SAFETY: a C struct of plain fields; all-zero is its "{}", then the
    // fields the C++ firmware sets.
    let mut config: esp_zigbee_config_t = unsafe { core::mem::zeroed() };
    config.device_config.device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR;
    config.device_config.install_code_policy = false;
    config.device_config.__bindgen_anon_1.zczr_config.max_children = 10;
    config.platform_config.storage_partition_name = c"nvs".as_ptr();
    config.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;

    unsafe {
        esp!(esp_zigbee_init(&config))?;
        setup_commissioning()?;
        create_endpoints()?;
    }
    {
        let (ready, cv) = &callbacks().ready;
        *ready.lock().unwrap() = true; // the lock exists from here on
        cv.notify_all();
    }
    unsafe {
        esp!(esp_zigbee_start(false))?;
        esp_zigbee_launch_mainloop();
        esp_zigbee_deinit();
    }
    Ok(())
}

unsafe fn setup_commissioning() -> Result<(), EspError> {
    unsafe {
        ezb_aps_secur_enable_distributed_security(false);
        esp!(ezb_bdb_set_primary_channel_set(PRIMARY_CHANNEL_MASK))?;
        esp!(ezb_bdb_set_secondary_channel_set(SECONDARY_CHANNEL_MASK))?;
        esp!(ezb_app_signal_add_handler(Some(on_signal)))?;
        ezb_apsde_data_indication_handler_register(Some(on_aps_indication));
    }
    Ok(())
}

unsafe fn create_endpoints() -> Result<(), EspError> {
    unsafe {
        let device = ezb_af_create_device_desc();

        let ctrl_cfg = sunsim_zha_color_dimmer_switch_config();
        let ctrl_ep = ezb_zha_create_color_dimmer_switch(LIGHT_CTRL_EP, &ctrl_cfg);
        let basic = ezb_af_endpoint_get_cluster_desc(ctrl_ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
        ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, MANUFACTURER_NAME.as_ptr().cast());
        ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, MODEL_IDENTIFIER.as_ptr().cast());
        esp!(ezb_af_device_add_endpoint_desc(device, ctrl_ep))?;

        let rx_cfg = sunsim_zha_on_off_light_config();
        let rx_ep = ezb_zha_create_on_off_light(BUTTON_RX_EP, &rx_cfg);
        esp!(ezb_af_device_add_endpoint_desc(device, rx_ep))?;

        esp!(ezb_af_device_desc_register(device))?;
        ezb_zcl_core_action_handler_register(Some(on_zcl_action));
    }
    Ok(())
}

// Commissioning again after `delay`, from the timer task (taking the lock).
fn schedule_commissioning_retry(mode: ezb_bdb_comm_mode_mask_t, delay: std::time::Duration) {
    let cb = callbacks();
    cb.retry_mode.store(mode, Ordering::Relaxed);
    let mut retry = cb.retry.lock().unwrap();
    if retry.is_none() {
        match cb.timers.timer(|| {
            let _stack = StackLock::acquire();
            unsafe { ezb_bdb_start_top_level_commissioning(callbacks().retry_mode.load(Ordering::Relaxed)) };
        }) {
            Ok(timer) => *retry = Some(timer),
            Err(e) => return log::error!(target: TAG, "No commissioning retry timer: {e}"),
        }
    }
    if let Some(timer) = retry.as_ref() {
        let _ = timer.cancel(); // harmless if not running
        let _ = timer.after(delay);
    }
}

// ---- Stack -> events (Zigbee task; the stack holds the lock) -------------------

unsafe extern "C" fn on_signal(signal: *const ezb_app_signal_t) -> bool {
    unsafe { handle_signal(signal) };
    true
}

unsafe fn handle_signal(signal: *const ezb_app_signal_t) {
    const RETRY: std::time::Duration = std::time::Duration::from_secs(1);
    let kind = unsafe { ezb_app_signal_get_type(signal) };
    let status = || unsafe { *params::<ezb_bdb_comm_status_t>(signal) };
    match kind {
        EZB_ZDO_SIGNAL_SKIP_STARTUP => {
            log::info!(target: TAG, "Initialize Zigbee stack");
            unsafe { ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION) };
        }
        EZB_BDB_SIGNAL_DEVICE_FIRST_START | EZB_BDB_SIGNAL_DEVICE_REBOOT => {
            if status() != EZB_BDB_STATUS_SUCCESS {
                log::warn!(target: TAG, "{} failed with status(0x{:02x}), retrying", signal_name(kind), status());
                return schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION, RETRY);
            }
            let factory_new = unsafe { ezb_bdb_is_factory_new() };
            log::info!(target: TAG, "Device started up in{} factory-reset mode", if factory_new { "" } else { " non" });
            if factory_new {
                unsafe { ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION) };
            } else {
                unsafe { ezb_bdb_open_network(DEFAULT_OPEN_SECS) };
                post(NetworkUp { rejoined: true, at: clock::now() });
            }
        }
        EZB_BDB_SIGNAL_FORMATION => {
            if status() != EZB_BDB_STATUS_SUCCESS {
                log::warn!(target: TAG, "Failed to form network with status(0x{:02x}), retrying", status());
                return schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_FORMATION, RETRY);
            }
            let mut ext_pan_id = ext_addr(0);
            unsafe {
                ezb_nwk_get_extended_panid(&mut ext_pan_id);
                log::info!(
                    target: TAG,
                    "Formed network: PAN(0x{:04x}, EXT:0x{:x}) Channel({}) Short(0x{:04x})",
                    ezb_nwk_get_panid(),
                    ext_of(&ext_pan_id),
                    ezb_nwk_get_current_channel(),
                    ezb_nwk_get_short_address()
                );
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            }
        }
        EZB_BDB_SIGNAL_STEERING => {
            if status() != EZB_BDB_STATUS_SUCCESS {
                log::warn!(target: TAG, "Steering failed with status(0x{:02x}), retrying", status());
                return schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING, RETRY);
            }
            log::info!(target: TAG, "Network formed. Type `pair <role>` (e.g. `pair WallA`), then reset that device.");
            unsafe { ezb_bdb_open_network(DEFAULT_OPEN_SECS) };
            post(NetworkUp { rejoined: false, at: clock::now() });
        }
        EZB_ZDO_SIGNAL_DEVICE_ANNCE => {
            let announce = unsafe { params::<ezb_zdo_signal_device_annce_params_t>(signal) };
            post(DeviceAnnounced { ext_addr: ext_of(&announce.device_addr), short_addr: announce.short_addr });
        }
        EZB_ZDO_SIGNAL_DEVICE_UPDATE => {
            let update = unsafe { params::<ezb_zdo_signal_device_update_params_t>(signal) };
            log::info!(
                target: TAG,
                "Device Update: ext=0x{:x} short=0x{:04x} status={} tc_action={} parent_short=0x{:04x}",
                ext_of(&update.device_addr),
                update.short_addr,
                update.status,
                update.tc_action,
                update.parent_short
            );
        }
        EZB_ZDO_SIGNAL_LEAVE => {
            let leave = unsafe { params::<ezb_zdo_signal_leave_params_t>(signal) };
            log::info!(target: TAG, "Device left network, type(0x{:02x})", leave.leave_type);
        }
        EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS => match unsafe { *params::<u8>(signal) } {
            0 => log::warn!(target: TAG, "Network closed to new devices"),
            secs => log::info!(target: TAG, "Network open for {secs} seconds"),
        },
        _ => log::info!(target: TAG, "Zigbee APP Signal: {}(0x{kind:02x})", signal_name(kind)),
    }
}

fn pressed(press: Press) {
    post(ButtonPressed { button: BUTTON, press, source: Source::Zigbee, at: clock::now() });
}

/// Buttons that do send to the bound receiver endpoint (BUTTON_RX_EP). The
/// stack resolves On/Off/Toggle itself and only surfaces the resulting
/// attribute, where On and Toggle look the same - hence Press::Ambiguous,
/// resolved by timing in the core's ModeController.
unsafe extern "C" fn on_zcl_action(callback_id: ezb_zcl_core_action_callback_id_t, message: *mut c_void) {
    // SAFETY: the callback id says which message type `message` is; it's the
    // stack's for the duration of the callback.
    unsafe {
        match callback_id {
            EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID => {
                let Some(m) = (message as *const ezb_zcl_set_attr_value_message_t).as_ref() else { return };
                if m.info.dst_ep == BUTTON_RX_EP && m.info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF {
                    let on = *(m.in_.attribute.data.value as *const u8) != 0;
                    log::info!(target: TAG, "Button On/Off attribute -> {on}");
                    pressed(if on { Press::Ambiguous } else { Press::Hold });
                }
            }
            EZB_ZCL_CORE_SCENES_RECALL_SCENE_CB_ID => {
                let Some(m) = (message as *const ezb_zcl_scenes_recall_scene_message_t).as_ref() else { return };
                if m.info.dst_ep == BUTTON_RX_EP {
                    log::info!(
                        target: TAG,
                        "Button Scenes RecallScene (group=0x{:04x} scene={}) -> treated as double-press",
                        m.in_.group_id,
                        m.in_.scene_id
                    );
                    pressed(Press::Double);
                }
            }
            EZB_ZCL_CORE_ON_OFF_OFF_WITH_EFFECT_CB_ID => {
                // Some switches send the extended OffWithEffect (0x40) for
                // their long-press/hold; the stack surfaces it separately from
                // the On/Off attribute change, so it needs its own case.
                let Some(m) = (message as *const ezb_zcl_on_off_off_with_effect_message_t).as_ref() else { return };
                if m.info.dst_ep == BUTTON_RX_EP {
                    log::info!(
                        target: TAG,
                        "Button OffWithEffect (effect={} variant={}) -> treated as long-press",
                        m.in_.effect_id,
                        m.in_.effect_variant
                    );
                    pressed(Press::Hold);
                }
            }
            EZB_ZCL_CORE_DEFAULT_RSP_CB_ID => {
                let Some(rsp) = (message as *const ezb_zcl_cmd_default_rsp_message_t).as_ref() else { return };
                if let Some(header) = rsp.in_.header.as_ref()
                    && let Some(short_addr) = short_of(&header.src_addr)
                {
                    post(AckReceived { short_addr, rssi: header.rssi, status: rsp.in_.status_code });
                }
                if rsp.in_.status_code != 0 {
                    // success acks arrive several times a second; only failures matter
                    log::warn!(target: TAG, "Default response status(0x{:02x})", rsp.in_.status_code);
                }
            }
            _ => {}
        }
    }
}

/// The paired button. SONOFF SNZB-01-style buttons say which press it was
/// (single = Toggle, double = On, hold = Off) and send it to our endpoint 1
/// whatever the binding says, where there's no On/Off server to receive it -
/// so it's picked out here, before the stack would drop it.
unsafe extern "C" fn on_aps_indication(ind: *const ezb_apsde_data_ind_t) -> bool {
    // SAFETY: the stack's, for the duration of the callback.
    let Some(ind) = (unsafe { ind.as_ref() }) else { return false };
    if ind.profile_id == 0 || ind.asdu_length < 3 || ind.asdu.is_null() {
        return false; // ZDO, or too short to be ZCL
    }
    // SAFETY: asdu_length bytes at asdu.
    let asdu = unsafe { std::slice::from_raw_parts(ind.asdu, ind.asdu_length as usize) };
    let frame_control = asdu[0];
    let mfr = frame_control & zcl::MANUFACTURER_SPECIFIC != 0;
    if mfr && asdu.len() < 5 {
        return false;
    }
    let cmd = asdu[if mfr { 4 } else { 2 }];
    let cluster_cmd = frame_control & zcl::FRAME_TYPE_MASK == zcl::CLUSTER_SPECIFIC;
    let src = short_of(&ind.src_address);

    // Diagnostics (enable DEBUG for the ZIGBEE tag to see a device's real
    // traffic), skipping the bulbs' routine ZCL Default Responses.
    if cluster_cmd || cmd != zcl::DEFAULT_RESPONSE {
        log::debug!(
            target: TAG,
            "RX from 0x{:04x} ep{} -> ep{} cluster 0x{:04x} {} cmd 0x{cmd:02x}{} rssi {} | {:02x?}",
            src.unwrap_or(0xFFFF),
            ind.src_endpoint,
            ind.dst_endpoint,
            ind.cluster_id,
            if cluster_cmd { "cluster" } else { "global" },
            if mfr { " (mfr-specific)" } else { "" },
            ind.rssi,
            &asdu[..asdu.len().min(16)]
        );
    }

    let button = callbacks().button_short.load(Ordering::Relaxed);
    if src != Some(button) || button == NO_BUTTON || ind.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF || !cluster_cmd || mfr
    {
        return false;
    }
    let press = match cmd {
        zcl::TOGGLE => ("single press (Toggle)", Press::Single),
        zcl::ON => ("double press (On)", Press::Double),
        zcl::OFF => ("hold (Off)", Press::Hold),
        _ => return false,
    };
    log::info!(target: TAG, "Button: {}", press.0);
    pressed(press.1);
    true // handled; don't let the stack reply "unsupported" to the button
}

unsafe extern "C" fn on_match_desc_result(result: *const ezb_zdo_match_desc_req_result_t, user_ctx: *mut c_void) {
    let role = role_from_ctx(user_ctx);
    // SAFETY: the stack's, for the duration of the callback.
    let result = unsafe { result.as_ref() };
    let rsp = result.and_then(|r| unsafe { response(r.error, r.rsp) });
    let endpoint = match rsp {
        Some(rsp) if rsp.status == EZB_ZDP_STATUS_SUCCESS && rsp.match_length > 0 => unsafe { *rsp.match_list },
        _ => {
            log::warn!(
                target: TAG,
                "Could not find {} endpoint for {} (err=0x{:x} status=0x{:x} matches={}), will retry",
                if is_button(role) { "On/Off output" } else { "light" },
                role_name(role),
                result.map_or(-1, |r| r.error),
                rsp.map_or(-1, |r| r.status as i32),
                rsp.map_or(-1, |r| r.match_length as i32)
            );
            0
        }
    };
    post(EndpointFound { role, endpoint });
}

unsafe extern "C" fn on_bind_result(result: *const ezb_zdp_bind_req_result_t, user_ctx: *mut c_void) {
    let ok = unsafe { result.as_ref() }
        .and_then(|r| unsafe { response(r.error, r.rsp) })
        .is_some_and(|rsp| rsp.status == EZB_ZDP_STATUS_SUCCESS);
    post(BindResult { role: role_from_ctx(user_ctx), ok });
}

unsafe extern "C" fn on_ping_result(result: *const ezb_zdo_active_ep_req_result_t, user_ctx: *mut c_void) {
    let result = unsafe { result.as_ref() };
    let rsp = result.and_then(|r| unsafe { response(r.error, r.rsp) });
    post(PingResult {
        role: role_from_ctx(user_ctx),
        ok: rsp.is_some_and(|r| r.status == EZB_ZDP_STATUS_SUCCESS),
        error: result.map_or(-1, |r| r.error),
        status: result.and_then(|r| unsafe { r.rsp.as_ref() }).map_or(-1, |r| r.status as i32),
    });
}
