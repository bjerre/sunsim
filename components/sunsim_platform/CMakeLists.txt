# sunsim::platform - everything that touches ESP-IDF: the Zigbee stack,
# NVS, the clock and Wi-Fi time sync, the console, and the display. Talks to
# the app logic (sunsim_core) only through events.
idf_component_register(SRCS "src/bulb_endpoint.cpp"
                            "src/clock.cpp"
                            "src/console.cpp"
                            "src/settings_store.cpp"
                            "src/status_display.cpp"
                            "src/wifi_time_sync.cpp"
                            "src/zigbee_adapter.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES sunsim_events esp_timer esp_event espressif__esp-zigbee-lib
                       PRIV_REQUIRES console driver esp_lcd nvs_flash esp_wifi esp_netif lwip
                                     espressif__esp_lvgl_port)
