# sunsim::core - all application logic, pure C++: no ESP-IDF calls, no
# radio, storage or clock access (those arrive and leave as events). Builds
# on the host too; see test/host.
idf_component_register(SRCS "src/core.cpp"
                            "src/daylight.cpp"
                            "src/daylight_program.cpp"
                            "src/day_info.cpp"
                            "src/device_registry.cpp"
                            "src/light_output.cpp"
                            "src/mode_controller.cpp"
                            "src/solar.cpp"
                            "src/status_model.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES sunsim_events)
