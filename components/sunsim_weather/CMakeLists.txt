# sunsim::weather - reading the weather services' replies (Open-Meteo, NWS)
# into sunsim::ev values, a chunk at a time as they arrive. Pure C++: the
# fetching is sunsim_platform's InternetSync. Builds on the host too; see
# test/host.
#
# The C++ module sunsim.weather: one .cppm per partition (weather.cppm the
# module's interface).
set(modules "src/weather.cppm"
            "src/json.cppm"
            "src/readers.cppm")

idf_component_register(SRCS ${modules}
                       REQUIRES sunsim_events)

target_sources(${COMPONENT_LIB} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${modules})
