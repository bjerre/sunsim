# Platform-neutral presentation data shared by the device display and the
# browser UI. No ESP-IDF dependencies.
set(modules "src/status_display.cppm")

idf_component_register(SRCS ${modules}
                       REQUIRES sunsim_events)

target_sources(${COMPONENT_LIB} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${modules})
