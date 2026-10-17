# sunsim::core - all application logic, pure C++: no ESP-IDF calls, no
# radio, storage or clock access (those arrive and leave as events). Builds
# on the host too; see test/host.
#
# The C++ module sunsim.core: one .cppm per partition (core.cppm the
# module's interface), each a class's declaration and its definitions.
set(modules "src/core.cppm"
            "src/device_registry.cppm"
            "src/light_output.cppm"
            "src/math/day_info.cppm"
            "src/math/daylight.cppm"
            "src/math/sky.cppm"
            "src/math/solar.cppm"
            "src/mode_controller.cppm"
            "src/overlays.cppm"
            "src/programs/daylight_program.cppm"
            "src/programs/off_program.cppm"
            "src/programs/program.cppm"
            "src/programs/program_host.cppm"
            "src/programs/red_alert_program.cppm"
            "src/programs/tuning_program.cppm"
            "src/status_model.cppm")

idf_component_register(SRCS ${modules}
                       REQUIRES sunsim_events)

target_sources(${COMPONENT_LIB} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${modules})
