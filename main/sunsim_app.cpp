#include "sunsim_app.h"

#include <cstdlib>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fast_day_clock.h"
#include "pairing_console.h"
#include "schedule_settings.h"
#include "sunsim_config.h"
#include "wifi_time_sync.h"

namespace sunsim {

namespace {
constexpr const char *kTag = "SUNSIM";

constexpr int kDiscoveryRetrySeconds = 15;
} // namespace

SunsimApp &SunsimApp::instance()
{
    static SunsimApp app;
    return app;
}

SunsimApp::SunsimApp()
    : light_rig_(DaylightSchedule{DaylightSchedule::Config{
          .latitude_deg  = std::strtod(CONFIG_SUNSIM_LATITUDE_DEG, nullptr),
          .longitude_deg = std::strtod(CONFIG_SUNSIM_LONGITUDE_DEG, nullptr),
      }}),
      button_(ButtonController::Callbacks{
          .resumeAuto    = [this] { light_rig_.snapAllBulbs(); },
          .manualOff     = [this] { light_rig_.forceAllOff(); },
          .enterRedAlert = [this] { light_rig_.driveRedAlertFlash(); },
          .redAlertTick  = [this] { light_rig_.driveRedAlertFlash(); },
      })
{
}

void SunsimApp::run()
{
    light_rig_.load();

    DaylightSchedule::Config schedule = light_rig_.scheduleConfig();
    loadScheduleSettings(schedule);
    light_rig_.setScheduleConfig(schedule);

#if CONFIG_SUNSIM_DEBUG_FAST_DAY
    ESP_LOGW(kTag, "DEBUG FAST DAY MODE: simulating one active window (work day) every %d seconds", CONFIG_SUNSIM_DEBUG_DAY_SECONDS);
    light_rig_.setClock([this, clock = FastDayClock{std::chrono::seconds{CONFIG_SUNSIM_DEBUG_DAY_SECONDS}}] {
        return clock.now(light_rig_.scheduleConfig());
    });
    daylight_tick_seconds_ = 1;
#endif
    light_rig_.setTickInterval(std::chrono::seconds{daylight_tick_seconds_});

#if !CONFIG_SUNSIM_DEBUG_FAST_DAY
    WifiTimeSync wifi_sync;
    wifi_sync.run();
#endif

    startHeartbeatTimer();

    ESP_LOGI(kTag, "Starting Zigbee stack");
    xTaskCreate(&SunsimApp::zigbeeTaskTrampoline, "Zigbee_main", 4096, this, 5, nullptr);

    startPairingConsole(light_rig_);
}

void SunsimApp::startHeartbeatTimer()
{
    const esp_timer_create_args_t args{
        .callback              = &SunsimApp::heartbeatTrampoline,
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "sunsim_heartbeat",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &heartbeat_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(heartbeat_timer_, 1'000'000));
}

void SunsimApp::heartbeatTick()
{
    // The esp_timer callback runs outside any Zigbee callback, so the
    // Zigbee SDK requires the lock held for the whole batch of commands
    // button_.tick()/applyAllBulbs() may issue below (see BulbEndpoint).
    esp_zigbee_lock_acquire(portMAX_DELAY);

    button_.tick();

    if (++discovery_retry_ticks_ >= kDiscoveryRetrySeconds) {
        discovery_retry_ticks_ = 0;
        light_rig_.retryMissingEndpoints();
    }

    if (++heartbeat_ticks_ >= daylight_tick_seconds_) {
        heartbeat_ticks_ = 0;
        if (button_.mode() == LightingMode::Auto) {
            light_rig_.applyAllBulbs();
        }
    }

    esp_zigbee_lock_release();
}

void SunsimApp::scheduleCommissioningRetry(ezb_bdb_comm_mode_mask_t mode, std::chrono::milliseconds delay)
{
    pending_retry_mode_ = mode;

    if (!retry_timer_) {
        const esp_timer_create_args_t args{
            .callback              = &SunsimApp::retryTrampoline,
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "sunsim_retry",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &retry_timer_));
    } else {
        esp_timer_stop(retry_timer_); // harmless if not running
    }
    esp_timer_start_once(retry_timer_, static_cast<uint64_t>(delay.count()) * 1000);
}

void SunsimApp::retryCommissioning()
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(pending_retry_mode_);
    esp_zigbee_lock_release();
}

void SunsimApp::handleSignal(const ezb_app_signal_t *app_signal)
{
    const ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(kTag, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const auto status = *static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(kTag, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ezb_bdb_open_network(180);
                light_rig_.snapAllBulbs();
            }
        } else {
            ESP_LOGW(kTag, "%s failed with status(0x%02x), retrying", ezb_app_signal_to_string(signal_type), status);
            scheduleCommissioningRetry(EZB_BDB_MODE_INITIALIZATION, std::chrono::milliseconds{1000});
        }
    } break;
    case EZB_BDB_SIGNAL_FORMATION: {
        const auto status = *static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t ext_pan_id;
            ezb_nwk_get_extended_panid(&ext_pan_id);
            ESP_LOGI(kTag, "Formed network: PAN(0x%04hx, EXT:0x%llx) Channel(%d) Short(0x%04hx)", ezb_nwk_get_panid(),
                     static_cast<unsigned long long>(ext_pan_id.u64), ezb_nwk_get_current_channel(),
                     ezb_nwk_get_short_address());
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(kTag, "Failed to form network with status(0x%02x), retrying", status);
            scheduleCommissioningRetry(EZB_BDB_MODE_NETWORK_FORMATION, std::chrono::milliseconds{1000});
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        const auto status = *static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(kTag, "Network formed. Type `pair <role>` (e.g. `pair WallA`), then reset that device.");
            ezb_bdb_open_network(180);
        } else {
            ESP_LOGW(kTag, "Steering failed with status(0x%02x), retrying", status);
            scheduleCommissioningRetry(EZB_BDB_MODE_NETWORK_STEERING, std::chrono::milliseconds{1000});
        }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const auto *params =
            static_cast<const ezb_zdo_signal_device_annce_params_t *>(ezb_app_signal_get_params(app_signal));
        light_rig_.handleDeviceAnnounce(params->device_addr.u64, params->short_addr);
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const auto *params =
            static_cast<const ezb_zdo_signal_device_update_params_t *>(ezb_app_signal_get_params(app_signal));
        ESP_LOGI(kTag, "Device Update: ext=0x%llx short=0x%04hx status=%d tc_action=%d parent_short=0x%04hx",
                 static_cast<unsigned long long>(params->device_addr.u64), params->short_addr, params->status,
                 params->tc_action, params->parent_short);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const auto *leave = static_cast<const ezb_zdo_signal_leave_params_t *>(ezb_app_signal_get_params(app_signal));
        ESP_LOGI(kTag, "Device left network, type(0x%02x)", leave->leave_type);
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const auto duration = *static_cast<const uint8_t *>(ezb_app_signal_get_params(app_signal));
        if (duration) {
            ESP_LOGI(kTag, "Network open for %d seconds", duration);
        } else {
            ESP_LOGW(kTag, "Network closed to new devices");
        }
    } break;
    default:
        ESP_LOGI(kTag, "Zigbee APP Signal: %s(0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
}

void SunsimApp::handleZclAction(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
        auto *m = static_cast<ezb_zcl_set_attr_value_message_t *>(message);
        if (m && m->info.dst_ep == SUNSIM_BUTTON_RX_EP_ID && m->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF) {
            const bool on = *static_cast<uint8_t *>(m->in.attribute.data.value) != 0;
            ESP_LOGI(kTag, "Button On/Off attribute -> %d", on);
            button_.onOffOffCommand(on);
        }
    } break;
    case EZB_ZCL_CORE_SCENES_RECALL_SCENE_CB_ID: {
        auto *m = static_cast<ezb_zcl_scenes_recall_scene_message_t *>(message);
        if (m && m->info.dst_ep == SUNSIM_BUTTON_RX_EP_ID) {
            ESP_LOGI(kTag, "Button Scenes RecallScene (group=0x%04x scene=%d) -> treated as double-press",
                     m->in.group_id, m->in.scene_id);
            button_.onSceneRecall();
        }
    } break;
    case EZB_ZCL_CORE_ON_OFF_OFF_WITH_EFFECT_CB_ID: {
        // Some switches send the extended OffWithEffect (0x40) command rather
        // than plain Off (0x00) for their long-press/hold action; the stack
        // surfaces it separately from EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID
        // instead of folding it into an On/Off attribute change, so it needs
        // its own case to not be silently dropped.
        auto *m = static_cast<ezb_zcl_on_off_off_with_effect_message_t *>(message);
        if (m && m->info.dst_ep == SUNSIM_BUTTON_RX_EP_ID) {
            ESP_LOGI(kTag, "Button OffWithEffect (effect=%d variant=%d) -> treated as long-press",
                     m->in.effect_id, m->in.effect_variant);
            button_.onOffOffCommand(false);
        }
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        auto *rsp = static_cast<ezb_zcl_cmd_default_rsp_message_t *>(message);
        if (rsp->in.status_code != 0) { // success acks arrive several times a second; only failures matter
            ESP_LOGW(kTag, "Default response status(0x%02x)", rsp->in.status_code);
        }
    } break;
    default:
        break;
    }
}

void SunsimApp::createEndpoints()
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();

    ezb_zha_color_dimmer_switch_config_t ctrl_cfg = EZB_ZHA_COLOR_DIMMER_SWITCH_CONFIG();
    ezb_af_ep_desc_t ctrl_ep = ezb_zha_create_color_dimmer_switch(SUNSIM_LIGHT_CTRL_EP_ID, &ctrl_cfg);
    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ctrl_ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                         (void *)SUNSIM_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                         (void *)SUNSIM_MODEL_IDENTIFIER);
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ctrl_ep));

    ezb_zha_on_off_light_config_t rx_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t rx_ep = ezb_zha_create_on_off_light(SUNSIM_BUTTON_RX_EP_ID, &rx_cfg);
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, rx_ep));

    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(&SunsimApp::zclTrampoline);
}

void SunsimApp::setupCommissioning()
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(SUNSIM_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(SUNSIM_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(&SunsimApp::signalTrampoline));
}

void SunsimApp::zigbeeTaskBody()
{
    esp_zigbee_config_t config = SUNSIM_ZB_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    setupCommissioning();
    createEndpoints();
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(nullptr);
}

bool SunsimApp::signalTrampoline(const ezb_app_signal_t *signal)
{
    instance().handleSignal(signal);
    return true;
}

void SunsimApp::zclTrampoline(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    instance().handleZclAction(callback_id, message);
}

void SunsimApp::heartbeatTrampoline(void *arg)
{
    static_cast<SunsimApp *>(arg)->heartbeatTick();
}

void SunsimApp::retryTrampoline(void *arg)
{
    static_cast<SunsimApp *>(arg)->retryCommissioning();
}

void SunsimApp::zigbeeTaskTrampoline(void *arg)
{
    static_cast<SunsimApp *>(arg)->zigbeeTaskBody();
}

} // namespace sunsim
