# sunsim::platform - everything that touches ESP-IDF: the Zigbee stack,
# NVS, the clock, the internet sync over Wi-Fi, the console, and the display. Talks to
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
            "src/internet_sync.cppm"
            "src/settings_store.cppm"
            "src/status_display.cppm"
            "src/status_slot.cppm"
            "src/wifi.cppm"
            "src/zigbee_adapter.cppm"
            "src/zone.cppm")

# InternetSync's and Clock's definitions live in module implementation units
# (see internet_sync.cppm and clock.cppm for why).
idf_component_register(SRCS ${modules} "src/clock.cpp" "src/internet_sync.cpp"
                       REQUIRES sunsim_events esp_timer esp_event espressif__esp-zigbee-lib
                       PRIV_REQUIRES sunsim_weather console driver pthread esp_coex esp_http_client esp_lcd mbedtls nvs_flash esp_wifi esp_netif lwip
                                     espressif__esp_lvgl_port)

target_sources(${COMPONENT_LIB} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${modules})
