# sunsim::core - all application logic, pure C++: no ESP-IDF calls, no
# radio, storage or clock access (those arrive and leave as events). Builds
# on the host too; see test/host.
#
# The C++ module sunsim.core: each src/*.cppm is a partition's interface
# (core.cppm the module's), each src/*.cpp its implementation.
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
                       REQUIRES sunsim_events)

target_sources(${COMPONENT_LIB} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
               "src/core.cppm"
               "src/device_registry.cppm"
               "src/light_output.cppm"
               "src/math/day_info.cppm"
               "src/math/daylight.cppm"
               "src/math/solar.cppm"
               "src/mode_controller.cppm"
               "src/programs/daylight_program.cppm"
               "src/programs/off_program.cppm"
               "src/programs/program.cppm"
               "src/programs/program_host.cppm"
               "src/programs/red_alert_program.cppm"
               "src/status_model.cppm")

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/import_std.cmake)
sunsim_import_std(${COMPONENT_LIB})
