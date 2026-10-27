module;

#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/zha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_SOC_IEEE802154_SUPPORTED
#error "sunsim requires a chip with a native 802.15.4 radio (e.g. ESP32-C6)"
#endif

export module sunsim.platform:zigbee_adapter;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :bulb_endpoint;

export namespace sunsim::platform {

// The only code that talks to esp-zigbee-lib. Runs the stack (its own task),
// forms/rejoins sunsim's network, and translates in both directions:
//   stack -> events : NetworkUp, DeviceAnnounced, EndpointFound, BindResult,
//                     PingResult, AckReceived, ButtonPressed
//   events -> stack : SetLight, Discover, Bind, Ping, OpenNetwork, FactoryReset
// The on() handlers run on the app task, which holds the stack's lock
// (StackLock) while processing events; the stack callbacks only post.
class ZigbeeAdapter {
public:
    explicit ZigbeeAdapter(ev::Post post);

    ZigbeeAdapter(const ZigbeeAdapter &)            = delete;
    ZigbeeAdapter &operator=(const ZigbeeAdapter &) = delete;

    // Spawns the Zigbee task. waitUntilReady() blocks until the stack (and
    // with it the Zigbee lock) exists.
    void start();
    void waitUntilReady();

    // The stack's API lock, for std::scoped_lock: the app task holds it
    // around event processing.
    struct StackLock {
        void lock();
        void unlock();
    };

    void on(const ev::SetLight &e);
    void on(const ev::Discover &e);
    void on(const ev::Bind &e);
    void on(const ev::Ping &e);
    void on(const ev::OpenNetwork &e);
    [[noreturn]] void on(const ev::FactoryReset &);
    void on(const ev::RoleAssigned &e);
    void on(const ev::RoleCleared &e);

private:
    void taskBody();
    void setupCommissioning();
    void createEndpoints();
    void handleSignal(const ezb_app_signal_t *signal);
    void handleZclAction(ezb_zcl_core_action_callback_id_t callback_id, void *message);
    bool handleApsIndication(const ezb_apsde_data_ind_t *ind);
    void scheduleCommissioningRetry(ezb_bdb_comm_mode_mask_t mode, std::chrono::milliseconds delay);
    void retryCommissioning();
    void pressed(ev::Press press);
    void countSend(ezb_err_t err);

    static void taskTrampoline(void *arg);
    static bool signalTrampoline(const ezb_app_signal_t *signal);
    static void zclTrampoline(ezb_zcl_core_action_callback_id_t callback_id, void *message);
    static bool apsTrampoline(const ezb_apsde_data_ind_t *ind);
    static void retryTrampoline(void *arg);
    static void onMatchDescResult(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx);
    static void onBindResult(const ezb_zdp_bind_req_result_t *result, void *user_ctx);
    static void onPingResult(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx);

    ev::Post                 post_;
    EventGroupHandle_t       ready_               = nullptr;
    esp_timer_handle_t       retry_timer_         = nullptr;
    ezb_bdb_comm_mode_mask_t pending_retry_mode_  = 0;

    // The button's short address (0xFFFF = none), read by the APS hook on the
    // Zigbee task to decide synchronously whether to consume a frame.
    std::atomic<uint16_t> button_short_{0xFFFF};

    // Bulb commands the stack refused to queue (e.g. out of buffers),
    // summarised every kSendReport so silent drops show up in the log.
    uint32_t    sends_           = 0;
    uint32_t    send_failures_   = 0;
    ezb_err_t   last_send_error_ = EZB_ERR_NONE;
    ev::Instant report_start_{};

    // For the stack's signal, ZCL and APS handlers, which take no context
    // pointer (its ZDO requests do: user_ctx carries the role).
    static ZigbeeAdapter *s_self;
};

} // namespace sunsim::platform

namespace sunsim::platform {

namespace {
constexpr const char *kTag = "ZIGBEE";

constexpr EventBits_t kReadyBit = BIT0;

constexpr auto kSendReport = std::chrono::seconds{10};

// Endpoint on which sunsim issues On/Off, Level and Color Control client
// commands to the bulbs.
constexpr uint8_t kLightCtrlEp = 1;

// Endpoint on which sunsim hosts an On/Off (+ Scenes) server, bound to the
// physical button controller so its presses arrive here.
constexpr uint8_t kButtonRxEp = 2;

// The button: there's one at most (ev::kLayout checks), and it's the one
// whose presses arrive. (With none, none arrive: any role will do.)
constexpr ev::Role kButton = [] {
    const auto button = std::ranges::find_if(ev::kRoles, ev::isButton);
    return button == ev::kRoles.end() ? ev::kRoles.front() : *button;
}();

constexpr uint32_t kPrimaryChannelMask   = 1U << CONFIG_SUNSIM_PRIMARY_CHANNEL;
constexpr uint32_t kSecondaryChannelMask = CONFIG_SUNSIM_SECONDARY_CHANNEL_MASK;

// A ZCL character string: a length byte, then the characters (no NUL).
template <size_t N>
consteval std::array<char, N> zclString(const char (&s)[N])
{
    std::array<char, N> out{static_cast<char>(N - 1)};
    std::copy_n(s, N - 1, out.begin() + 1);
    return out;
}

constexpr auto kManufacturerName = zclString("sunsim");
constexpr auto kModelIdentifier  = zclString(CONFIG_IDF_TARGET);

long long secondsOf(ev::BootClock::duration d)
{
    return std::chrono::duration_cast<std::chrono::seconds>(d).count();
}

// A role through a ZDO request's user_ctx, as its slot.
void *roleCtx(ev::Role role)
{
    return reinterpret_cast<void *>(static_cast<intptr_t>(ev::slot(role)));
}

ev::Role roleFromCtx(void *ctx)
{
    return ev::kRoles[static_cast<std::size_t>(reinterpret_cast<intptr_t>(ctx))];
}

// A signal's parameters, as the type the signal carries.
template <class T> const T &params(const ezb_app_signal_t *signal)
{
    return *static_cast<const T *>(ezb_app_signal_get_params(signal));
}

// The ZCL frame header's fields that pick out the button's commands (ZCL
// spec 2.4.1): the frame control byte, then (if manufacturer-specific) a
// 2-byte manufacturer code, the sequence number, and the command.
namespace zcl {
constexpr uint8_t kFrameTypeMask        = 0x03; // frame control bits 0-1
constexpr uint8_t kClusterSpecific      = 0x01; // ... a cluster's own command, not a global one
constexpr uint8_t kManufacturerSpecific = 0x04; // frame control bit 2
constexpr uint8_t kDefaultResponse      = 0x0B; // the global command

enum class OnOff : uint8_t { Off = 0x00, On = 0x01, Toggle = 0x02 }; // On/Off cluster commands
} // namespace zcl

// A ZDO request got through and the device answered SUCCESS.
template <class Result> bool zdoOk(const Result *result)
{
    return result && result->error == EZB_ERR_NONE && result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS;
}
} // namespace

ZigbeeAdapter *ZigbeeAdapter::s_self = nullptr;

ZigbeeAdapter::ZigbeeAdapter(ev::Post post) : post_(std::move(post)), ready_(xEventGroupCreate())
{
    s_self = this;
}

void ZigbeeAdapter::start()
{
    ev::log::info(kTag, "Starting Zigbee stack");
    // 6 KB: the stack's callbacks run here and post events (~256 B per copy).
    xTaskCreate(&ZigbeeAdapter::taskTrampoline, "Zigbee_main", 6144, this, 5, nullptr);
}

void ZigbeeAdapter::waitUntilReady()
{
    xEventGroupWaitBits(ready_, kReadyBit, pdFALSE, pdTRUE, portMAX_DELAY);
}

void ZigbeeAdapter::StackLock::lock()
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
}

void ZigbeeAdapter::StackLock::unlock()
{
    esp_zigbee_lock_release();
}

// ---- Stack lifecycle ----------------------------------------------------------

void ZigbeeAdapter::taskBody()
{
    esp_zigbee_config_t config{
        .device_config =
            {
                .device_type         = EZB_NWK_DEVICE_TYPE_COORDINATOR,
                .install_code_policy = false,
                .zczr_config         = {.max_children = 10},
            },
        .platform_config =
            {
                .storage_partition_name = "nvs",
                .radio_config =
                    {
                        .radio_mode        = ESP_ZIGBEE_RADIO_MODE_NATIVE,
                        .radio_uart_config = {},
                    },
            },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    setupCommissioning();
    createEndpoints();
    xEventGroupSetBits(ready_, kReadyBit); // the lock exists from here on
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(nullptr);
}

void ZigbeeAdapter::setupCommissioning()
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(kPrimaryChannelMask));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(kSecondaryChannelMask));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(&ZigbeeAdapter::signalTrampoline));
    ezb_apsde_data_indication_handler_register(&ZigbeeAdapter::apsTrampoline);
}

void ZigbeeAdapter::createEndpoints()
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();

    ezb_zha_color_dimmer_switch_config_t ctrl_cfg = EZB_ZHA_COLOR_DIMMER_SWITCH_CONFIG();
    ezb_af_ep_desc_t ctrl_ep = ezb_zha_create_color_dimmer_switch(kLightCtrlEp, &ctrl_cfg);
    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ctrl_ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                         const_cast<char *>(kManufacturerName.data()));
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                         const_cast<char *>(kModelIdentifier.data()));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ctrl_ep));

    ezb_zha_on_off_light_config_t rx_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t rx_ep = ezb_zha_create_on_off_light(kButtonRxEp, &rx_cfg);
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, rx_ep));

    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(&ZigbeeAdapter::zclTrampoline);
}

void ZigbeeAdapter::scheduleCommissioningRetry(ezb_bdb_comm_mode_mask_t mode, std::chrono::milliseconds delay)
{
    pending_retry_mode_ = mode;

    if (!retry_timer_) {
        const esp_timer_create_args_t args{
            .callback              = &ZigbeeAdapter::retryTrampoline,
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "zb_retry",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &retry_timer_));
    } else {
        esp_timer_stop(retry_timer_); // harmless if not running
    }
    esp_timer_start_once(retry_timer_, static_cast<uint64_t>(delay.count()) * 1000);
}

void ZigbeeAdapter::retryCommissioning()
{
    StackLock               stack;
    const std::scoped_lock lock{stack};
    ezb_bdb_start_top_level_commissioning(pending_retry_mode_);
}

// ---- Stack -> events (Zigbee task; the stack holds the lock) -------------------

void ZigbeeAdapter::handleSignal(const ezb_app_signal_t *app_signal)
{
    const ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ev::log::info(kTag, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const auto status = params<ezb_bdb_comm_status_t>(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ev::log::info(kTag, "Device started up in{} factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ezb_bdb_open_network(ev::kDefaultOpenTime.count());
                post_(ev::NetworkUp{.rejoined = true, .at = ev::BootClock::now()});
            }
        } else {
            ev::log::warn(kTag, "{} failed with status(0x{:02x}), retrying", ezb_app_signal_to_string(signal_type), status);
            scheduleCommissioningRetry(EZB_BDB_MODE_INITIALIZATION, std::chrono::milliseconds{1000});
        }
    } break;
    case EZB_BDB_SIGNAL_FORMATION: {
        const auto status = params<ezb_bdb_comm_status_t>(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t ext_pan_id;
            ezb_nwk_get_extended_panid(&ext_pan_id);
            ev::log::info(kTag, "Formed network: PAN(0x{:04x}, EXT:0x{:x}) Channel({}) Short(0x{:04x})", ezb_nwk_get_panid(),
                          uint64_t{ext_pan_id.u64}, ezb_nwk_get_current_channel(),
                          ezb_nwk_get_short_address());
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ev::log::warn(kTag, "Failed to form network with status(0x{:02x}), retrying", status);
            scheduleCommissioningRetry(EZB_BDB_MODE_NETWORK_FORMATION, std::chrono::milliseconds{1000});
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        const auto status = params<ezb_bdb_comm_status_t>(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ev::log::info(kTag, "Network formed. Type `pair <role>` (e.g. `pair WallA`), then reset that device.");
            ezb_bdb_open_network(ev::kDefaultOpenTime.count());
            post_(ev::NetworkUp{.rejoined = false, .at = ev::BootClock::now()});
        } else {
            ev::log::warn(kTag, "Steering failed with status(0x{:02x}), retrying", status);
            scheduleCommissioningRetry(EZB_BDB_MODE_NETWORK_STEERING, std::chrono::milliseconds{1000});
        }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const auto &announce = params<ezb_zdo_signal_device_annce_params_t>(app_signal);
        post_(ev::DeviceAnnounced{.ext_addr = announce.device_addr.u64, .short_addr = announce.short_addr});
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const auto &update = params<ezb_zdo_signal_device_update_params_t>(app_signal);
        ev::log::info(kTag, "Device Update: ext=0x{:x} short=0x{:04x} status={} tc_action={} parent_short=0x{:04x}",
                      update.device_addr.u64, update.short_addr, update.status,
                      update.tc_action, update.parent_short);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        ev::log::info(kTag, "Device left network, type(0x{:02x})", params<ezb_zdo_signal_leave_params_t>(app_signal).leave_type);
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const auto duration = params<uint8_t>(app_signal);
        if (duration) {
            ev::log::info(kTag, "Network open for {} seconds", duration);
        } else {
            ev::log::warn(kTag, "Network closed to new devices");
        }
    } break;
    default:
        ev::log::info(kTag, "Zigbee APP Signal: {}(0x{:02x})", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
}

void ZigbeeAdapter::pressed(ev::Press press)
{
    post_(ev::ButtonPressed{.button = kButton,
                            .press  = press,
                            .source = ev::Source::Zigbee,
                            .at     = ev::BootClock::now()});
}

// Buttons that do send to the bound receiver endpoint (kButtonRxEp).
// The stack resolves On/Off/Toggle itself and only surfaces the resulting
// attribute, where On and Toggle look the same - hence Press::Ambiguous,
// resolved by timing in core::ModeController.
void ZigbeeAdapter::handleZclAction(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
        auto *m = static_cast<ezb_zcl_set_attr_value_message_t *>(message);
        if (m && m->info.dst_ep == kButtonRxEp && m->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF) {
            const bool on = *static_cast<uint8_t *>(m->in.attribute.data.value) != 0;
            ev::log::info(kTag, "Button On/Off attribute -> {}", on);
            pressed(on ? ev::Press::Ambiguous : ev::Press::Hold);
        }
    } break;
    case EZB_ZCL_CORE_SCENES_RECALL_SCENE_CB_ID: {
        auto *m = static_cast<ezb_zcl_scenes_recall_scene_message_t *>(message);
        if (m && m->info.dst_ep == kButtonRxEp) {
            ev::log::info(kTag, "Button Scenes RecallScene (group=0x{:04x} scene={}) -> treated as double-press",
                          m->in.group_id, m->in.scene_id);
            pressed(ev::Press::Double);
        }
    } break;
    case EZB_ZCL_CORE_ON_OFF_OFF_WITH_EFFECT_CB_ID: {
        // Some switches send the extended OffWithEffect (0x40) for their
        // long-press/hold; the stack surfaces it separately from the On/Off
        // attribute change, so it needs its own case.
        auto *m = static_cast<ezb_zcl_on_off_off_with_effect_message_t *>(message);
        if (m && m->info.dst_ep == kButtonRxEp) {
            ev::log::info(kTag, "Button OffWithEffect (effect={} variant={}) -> treated as long-press",
                          m->in.effect_id, m->in.effect_variant);
            pressed(ev::Press::Hold);
        }
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        auto *rsp = static_cast<ezb_zcl_cmd_default_rsp_message_t *>(message);
        if (rsp->in.header && rsp->in.header->src_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
            post_(ev::AckReceived{.short_addr = rsp->in.header->src_addr.u.short_addr,
                                  .rssi       = rsp->in.header->rssi,
                                  .status     = rsp->in.status_code});
        }
        if (rsp->in.status_code != 0) { // success acks arrive several times a second; only failures matter
            ev::log::warn(kTag, "Default response status(0x{:02x})", rsp->in.status_code);
        }
    } break;
    default:
        break;
    }
}

bool ZigbeeAdapter::handleApsIndication(const ezb_apsde_data_ind_t *ind)
{
    if (!ind || ind->profile_id == 0 || ind->asdu_length < 3) {
        return false; // ZDO, or too short to be ZCL
    }
    const uint8_t frame_control = ind->asdu[0];
    const bool    mfr           = frame_control & zcl::kManufacturerSpecific;
    if (mfr && ind->asdu_length < 5) {
        return false;
    }
    const uint8_t  cmd         = ind->asdu[mfr ? 4 : 2];
    const bool     cluster_cmd = (frame_control & zcl::kFrameTypeMask) == zcl::kClusterSpecific;
    const bool     from_short  = ind->src_address.addr_mode == EZB_ADDR_MODE_SHORT;
    const uint16_t src         = from_short ? ind->src_address.u.short_addr : 0xffff;

    // Diagnostics (enable DEBUG for the ZIGBEE tag to see a device's real
    // traffic), skipping the bulbs' routine ZCL Default Responses.
    if (cluster_cmd || cmd != zcl::kDefaultResponse) {
        const std::span frame{ind->asdu, std::min<std::size_t>(ind->asdu_length, 16)};
        ev::log::debug(kTag, "RX from 0x{:04x} ep{} -> ep{} cluster 0x{:04x} {} cmd 0x{:02x}{} rssi {} | {::02x}", src,
                       ind->src_endpoint, ind->dst_endpoint, ind->cluster_id, cluster_cmd ? "cluster" : "global", cmd,
                       mfr ? " (mfr-specific)" : "", ind->rssi, frame);
    }

    // The paired button. SONOFF SNZB-01-style buttons say which press it was
    // (single = Toggle, double = On, hold = Off) and send it to our endpoint
    // 1 whatever the binding says, where there's no On/Off server to receive
    // it - so it's picked out here, before the stack would drop it.
    if (!from_short || src != button_short_.load() || ind->cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF || !cluster_cmd ||
        mfr) {
        return false;
    }
    switch (static_cast<zcl::OnOff>(cmd)) {
    case zcl::OnOff::Toggle:
        ev::log::info(kTag, "Button: single press (Toggle)");
        pressed(ev::Press::Single);
        break;
    case zcl::OnOff::On:
        ev::log::info(kTag, "Button: double press (On)");
        pressed(ev::Press::Double);
        break;
    case zcl::OnOff::Off:
        ev::log::info(kTag, "Button: hold (Off)");
        pressed(ev::Press::Hold);
        break;
    default:
        return false;
    }
    return true; // handled; don't let the stack reply "unsupported" to the button
}

void ZigbeeAdapter::onMatchDescResult(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    const ev::Role role = roleFromCtx(user_ctx);
    if (!zdoOk(result) || result->rsp->match_length == 0) {
        ev::log::warn(kTag, "Could not find {} endpoint for {} (err=0x{:x} status=0x{:x} matches={}), will retry",
                      ev::isButton(role) ? "On/Off output" : "light", ev::roleName(role).data(),
                      result ? result->error : -1, (result && result->rsp) ? result->rsp->status : -1,
                      (result && result->rsp) ? result->rsp->match_length : -1);
        s_self->post_(ev::EndpointFound{.role = role, .endpoint = 0});
        return;
    }
    s_self->post_(ev::EndpointFound{.role = role, .endpoint = result->rsp->match_list[0]});
}

void ZigbeeAdapter::onBindResult(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    s_self->post_(ev::BindResult{.role = roleFromCtx(user_ctx), .ok = zdoOk(result)});
}

void ZigbeeAdapter::onPingResult(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    s_self->post_(ev::PingResult{.role   = roleFromCtx(user_ctx),
                                 .ok     = zdoOk(result),
                                 .error  = result ? static_cast<int>(result->error) : -1,
                                 .status = (result && result->rsp) ? static_cast<int>(result->rsp->status) : -1});
}

// ---- Events -> stack (app task; it holds the lock) ------------------------------

void ZigbeeAdapter::on(const ev::SetLight &e)
{
    const BulbEndpoint bulb{e.short_addr, e.endpoint, kLightCtrlEp};

    // A saturated color goes before the level, whites after it.
    const auto *hue_sat = e.color ? std::get_if<ev::HueSat>(&*e.color) : nullptr;
    if (e.power && *e.power) {
        countSend(bulb.turnOn());
    }
    if (hue_sat) {
        countSend(bulb.setHueSaturation(hue_sat->hue, hue_sat->saturation, e.color_transition));
    }
    if (e.level) {
        countSend(bulb.setLevel(*e.level, e.level_transition));
    }
    if (const auto *xy = e.color ? std::get_if<ev::Xy>(&*e.color) : nullptr) {
        countSend(bulb.setColorXY(xy->x, xy->y, e.color_transition));
    } else if (const auto *mireds = e.color ? std::get_if<ev::Mireds>(&*e.color) : nullptr) {
        countSend(bulb.setColorTemperature(mireds->value, e.color_transition));
    }
    if (e.power && !*e.power) {
        countSend(bulb.turnOff());
    }
}

void ZigbeeAdapter::countSend(ezb_err_t err)
{
    ++sends_;
    if (err != EZB_ERR_NONE) {
        ++send_failures_;
        last_send_error_ = err;
    }
    const ev::Instant now = ev::BootClock::now();
    if (now - report_start_ >= kSendReport) {
        if (send_failures_ > 0) {
            ev::log::warn(kTag, "{} of {} bulb commands not queued in the last {}s (last err 0x{:x})",
                          send_failures_, sends_,
                          secondsOf(now - report_start_), last_send_error_);
        } else {
            ev::log::debug(kTag, "{} bulb commands sent in the last {}s", sends_,
                           secondsOf(now - report_start_));
        }
        sends_           = 0;
        send_failures_   = 0;
        report_start_    = now;
    }
}

void ZigbeeAdapter::on(const ev::Discover &e)
{
    ezb_zdo_match_desc_req_t req{};
    req.dst_nwk_addr               = e.short_addr;
    req.field.nwk_addr_of_interest = e.short_addr;
    req.field.profile_id           = EZB_AF_HA_PROFILE_ID;
    req.cb                         = &ZigbeeAdapter::onMatchDescResult;
    req.user_ctx                   = roleCtx(e.role);
    // Its endpoint with any of these: a light's server clusters (what it
    // takes), a button's On/Off client (what it sends).
    static uint16_t color_bulb[] = {EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL};
    static uint16_t on_off[]     = {EZB_ZCL_CLUSTER_ID_ON_OFF};
    const auto      in = [&](std::span<uint16_t> clusters) {
        req.field.num_in_clusters = static_cast<uint8_t>(clusters.size());
        req.field.cluster_list    = clusters.data();
    };
    switch (ev::kindOf(e.role)) {
    case ev::Kind::ColorBulb:
        in(color_bulb);
        break;
    case ev::Kind::Plug:
        in(on_off);
        break;
    case ev::Kind::Button:
        req.field.num_out_clusters = 1;
        req.field.cluster_list     = on_off;
        break;
    }
    ezb_zdo_match_desc_req(&req);
}

void ZigbeeAdapter::on(const ev::Bind &e)
{
    ezb_extaddr_t self_ext;
    ezb_get_extended_address(&self_ext);
    ezb_extaddr_t device_ext{};
    device_ext.u64 = e.ext_addr;

    ezb_zdo_bind_req_t req{};
    req.dst_nwk_addr                 = e.short_addr;
    req.field.src_addr               = device_ext;
    req.field.src_ep                 = e.endpoint;
    req.field.cluster_id             = EZB_ZCL_CLUSTER_ID_ON_OFF;
    req.field.dst_addr_mode          = EZB_ADDR_MODE_EXT;
    req.field.dst_addr.extended_addr = self_ext;
    req.field.dst_ep                 = kButtonRxEp;
    req.cb                           = &ZigbeeAdapter::onBindResult;
    req.user_ctx                     = roleCtx(e.role);
    ezb_zdo_bind_req(&req);
}

void ZigbeeAdapter::on(const ev::Ping &e)
{
    ezb_zdo_active_ep_req_t req{};
    req.dst_nwk_addr               = e.short_addr;
    req.field.nwk_addr_of_interest = e.short_addr;
    req.cb                         = &ZigbeeAdapter::onPingResult;
    req.user_ctx                   = roleCtx(e.role);
    ezb_zdo_active_ep_req(&req);
}

void ZigbeeAdapter::on(const ev::OpenNetwork &e)
{
    const ezb_err_t err = ezb_bdb_open_network(e.duration.count());
    if (err != EZB_ERR_NONE) {
        ev::log::warn(kTag, "Could not open network (err 0x{:x}) - has it finished forming?", err);
    }
}

void ZigbeeAdapter::on(const ev::FactoryReset &)
{
    ev::log::warn(kTag, "Clearing the Zigbee network, then restarting");
    esp_zigbee_factory_reset(); // does not return
}

void ZigbeeAdapter::on(const ev::RoleAssigned &e)
{
    if (ev::isButton(e.role)) {
        button_short_.store(e.record.valid ? e.record.short_addr : 0xFFFF);
    }
}

void ZigbeeAdapter::on(const ev::RoleCleared &e)
{
    if (ev::isButton(e.role)) {
        button_short_.store(0xFFFF);
    }
}

// ---- Trampolines --------------------------------------------------------------

void ZigbeeAdapter::taskTrampoline(void *arg)
{
    static_cast<ZigbeeAdapter *>(arg)->taskBody();
}

bool ZigbeeAdapter::signalTrampoline(const ezb_app_signal_t *signal)
{
    s_self->handleSignal(signal);
    return true;
}

void ZigbeeAdapter::zclTrampoline(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    s_self->handleZclAction(callback_id, message);
}

bool ZigbeeAdapter::apsTrampoline(const ezb_apsde_data_ind_t *ind)
{
    return s_self->handleApsIndication(ind);
}

void ZigbeeAdapter::retryTrampoline(void *arg)
{
    static_cast<ZigbeeAdapter *>(arg)->retryCommissioning();
}

} // namespace sunsim::platform
