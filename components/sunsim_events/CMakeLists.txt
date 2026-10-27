# sunsim::ev - the event vocabulary shared by every layer: value types,
# event payloads, coroutine flows, the EventBus, the settings table and
# logging. No ESP-IDF dependencies beyond logging and the boot clock, so it also builds on the host (see
# test/host).
#
# The C++ module sunsim.ev: one .cppm per partition (ev.cppm the module's
# interface), plus `import std;` for every component - they all require
# this one.
set(modules "src/ev.cppm"
            "src/bus.cppm"
            "src/events.cppm"
            "src/flow.cppm"
            "src/installation.cppm"
            "src/layout.cppm"
            "src/log.cppm"
            "src/settings.cppm"
            "src/topology.cppm"
            "src/types.cppm")

idf_component_register(SRCS ${modules}
                       REQUIRES log
                       PRIV_REQUIRES esp_timer)

target_sources(${COMPONENT_LIB} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${modules})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/import_std.cmake)
sunsim_import_std(${COMPONENT_LIB})
