#pragma once

#define SUNSIM_PRIMARY_CHANNEL_MASK   (1U << CONFIG_SUNSIM_PRIMARY_CHANNEL)
#define SUNSIM_SECONDARY_CHANNEL_MASK CONFIG_SUNSIM_SECONDARY_CHANNEL_MASK

// Endpoint on which sunsim issues On/Off, Level and Color Control client
// commands to the 4 bulbs.
#define SUNSIM_LIGHT_CTRL_EP_ID (1)

// Endpoint on which sunsim hosts an On/Off (+ Scenes) server, bound to the
// physical button controller so its presses arrive here.
#define SUNSIM_BUTTON_RX_EP_ID (2)

#define SUNSIM_STORAGE_PARTITION_NAME "nvs"

#define SUNSIM_MANUFACTURER_NAME "\x06"                    "sunsim"
#define SUNSIM_MODEL_IDENTIFIER  "\x07" CONFIG_IDF_TARGET

#define SUNSIM_ZC_CONFIG()                                \
    {                                                      \
        .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,   \
        .install_code_policy = false,                     \
        .zczr_config = {                                  \
            .max_children = 10,                           \
        },                                                 \
    }

#if CONFIG_SOC_IEEE802154_SUPPORTED
#define SUNSIM_PLATFORM_CONFIG()                                 \
    {                                                             \
        .storage_partition_name = SUNSIM_STORAGE_PARTITION_NAME, \
        .radio_config = {                                        \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,           \
            .radio_uart_config = {},                              \
        },                                                        \
    }
#else
#error "sunsim requires a chip with a native 802.15.4 radio (e.g. ESP32-C6)"
#endif

#define SUNSIM_ZB_DEFAULT_CONFIG()                     \
    {                                                   \
        .device_config = SUNSIM_ZC_CONFIG(),           \
        .platform_config = SUNSIM_PLATFORM_CONFIG(),   \
    }
