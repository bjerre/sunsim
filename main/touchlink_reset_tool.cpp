#include "touchlink_reset_tool.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/touchlink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sunsim_config.h"

namespace sunsim {

namespace {
constexpr const char *kTag = "TOUCHLINK_RESET";

void scanTick(void *)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    const ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_TOUCHLINK_INITIATOR);
    esp_zigbee_lock_release();
    if (err == EZB_ERR_NONE) {
        ESP_LOGI(kTag, "Scanning for a nearby Touchlink device...");
    } else {
        ESP_LOGW(kTag, "Could not start scan, err(0x%x) - likely still busy from the previous attempt", err);
    }
}

void startPeriodicScanning()
{
    static esp_timer_handle_t timer{};
    if (timer) {
        return; // already started (e.g. a second DEVICE_REBOOT signal)
    }
    const esp_timer_create_args_t args{
        .callback              = &scanTick,
        .arg                   = nullptr,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "touchlink_scan",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 8'000'000)); // retry every 8s
    scanTick(nullptr); // don't wait for the first tick
}

bool touchlinkActionPermission(ezb_touchlink_action_t action, const ezb_extaddr_t *ieee_addr)
{
    if (action == EZB_TOUCHLINK_ACTION_SELECTED_TARGET) {
        ESP_LOGW(kTag, "Found device ext=0x%llx - sending reset-to-factory-new",
                 static_cast<unsigned long long>(ieee_addr->u64));
        ezb_touchlink_send_reset_to_factory_new_req(ieee_addr);
    }
    // Never actually join/commission the device onto our network - this
    // tool only wants the proximity-triggered factory reset.
    return false;
}

bool signalHandler(const ezb_app_signal_t *app_signal)
{
    switch (ezb_app_signal_get_type(app_signal)) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const auto status = *static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(kTag, "Ready - hold the board within a few cm of a bulb to reset it");
            startPeriodicScanning();
        } else {
            ESP_LOGE(kTag, "Zigbee init failed with status(0x%02x) - power-cycle the board and reflash", status);
        }
    } break;
    case EZB_BDB_SIGNAL_TOUCHLINK_INITIATOR_FINISHED: {
        const auto status = *static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_NO_SCAN_RESPONSE) {
            ESP_LOGI(kTag, "Scan finished: no response - nothing nearby");
        } else {
            ESP_LOGI(kTag, "Scan finished, status(0x%02x)", status);
        }
    } break;
    default:
        break;
    }
    return true;
}

[[noreturn]] void zigbeeTaskBody(void *)
{
    esp_zigbee_config_t config = SUNSIM_ZB_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(SUNSIM_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(SUNSIM_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(&signalHandler));
    ezb_touchlink_action_permission_handler_register(&touchlinkActionPermission);
    ezb_touchlink_set_key_bitmask(EZB_TOUCHLINK_KEY_BITMASK_DEVELOPMENT | EZB_TOUCHLINK_KEY_BITMASK_MASTER |
                                   EZB_TOUCHLINK_KEY_BITMASK_CERTIFICATION);
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(nullptr);
    __builtin_unreachable();
}

} // namespace

void runTouchlinkResetTool()
{
    ESP_LOGW(kTag, "Touchlink factory-reset tool active.");
    ESP_LOGW(kTag, "Hold the board within a few cm of one bulb at a time - Touchlink uses");
    ESP_LOGW(kTag, "reduced TX power on purpose so only the nearest device responds.");
    xTaskCreate(&zigbeeTaskBody, "Touchlink_main", 4096, nullptr, 5, nullptr);
}

} // namespace sunsim
