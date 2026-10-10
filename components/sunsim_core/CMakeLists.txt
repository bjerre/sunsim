# sunsim::core - all application logic, pure C++: no ESP-IDF calls, no
# radio, storage or clock access (those arrive and leave as events). Builds
# on the host too; see test/host.
idf_component_register(SRCS "src/core.cpp"
                            "src/device_registry.cpp"
                            "src/light_output.cpp"
                            "src/math/day_info.cpp"
                            "src/math/daylight.cpp"
                            "src/math/solar.cpp"
                            "src/mode_controller.cpp"
                            "src/programs/daylight_program.cpp"
                            "src/programs/off_program.cpp"
                            "src/programs/program_host.cpp"
                            "src/programs/red_alert_program.cpp"
                            "src/status_model.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES sunsim_events)
