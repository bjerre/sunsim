# sunsim::platform - everything that touches ESP-IDF: the Zigbee stack,
# NVS, the clock and Wi-Fi time sync, the console, and the display. Talks to
# the app logic (sunsim_core) only through events.
#
# The C++ module sunsim.platform: one .cppm per partition (platform.cppm the
# module's interface), each a class's declaration and its definitions.
set(modules "src/platform.cppm"
            "src/bulb_endpoint.cppm"
            "src/clock.cppm"
            "src/console.cppm"
            "src/fast_day_clock.cppm"
            "src/format.cppm"
            "src/settings_store.cppm"
            "src/status_display.cppm"
            "src/status_slot.cppm"
            "src/wifi_time_sync.cppm"
            "src/zigbee_adapter.cppm")

idf_component_register(SRCS ${modules}
                       REQUIRES sunsim_events esp_timer esp_event espressif__esp-zigbee-lib
                       PRIV_REQUIRES console driver esp_lcd nvs_flash esp_wifi esp_netif lwip
                                     espressif__esp_lvgl_port)

target_sources(${COMPONENT_LIB} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${modules})
