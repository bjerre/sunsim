# Host tests for the pure layers (sunsim_events, sunsim_core, sunsim_weather): plain CMake,
# no ESP-IDF. Built with the firmware's C++ settings (no exceptions, no RTTI).
# C++ modules need the Ninja generator (ESP-IDF's export.sh puts one on PATH):
#
#   cmake -S test/host -B build-host -G Ninja && cmake --build build-host && ./build-host/sunsim_tests
cmake_minimum_required(VERSION 3.28)
project(sunsim_host_tests CXX)

set(CMAKE_CXX_STANDARD 26) # the firmware builds as gnu++26 (ESP-IDF 6.1)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${ROOT}/cmake/import_std.cmake)

# The modules sunsim.ev, sunsim.core and sunsim.weather, as the firmware's components build them.
set(EVENTS ${ROOT}/components/sunsim_events)
file(GLOB EVENTS_MODULES CONFIGURE_DEPENDS ${EVENTS}/src/*.cppm)
add_library(sunsim_events STATIC)
target_sources(sunsim_events PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${EVENTS}/src FILES ${EVENTS_MODULES})
target_include_directories(sunsim_events PUBLIC ${EVENTS}/include)
sunsim_import_std(sunsim_events)

set(CORE ${ROOT}/components/sunsim_core/src)
file(GLOB_RECURSE CORE_MODULES CONFIGURE_DEPENDS ${CORE}/*.cppm)
add_library(sunsim_core STATIC)
target_sources(sunsim_core PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CORE} FILES ${CORE_MODULES})
target_link_libraries(sunsim_core PUBLIC sunsim_events)

set(WEATHER ${ROOT}/components/sunsim_weather/src)
file(GLOB WEATHER_MODULES CONFIGURE_DEPENDS ${WEATHER}/*.cppm)
add_library(sunsim_weather STATIC)
target_sources(sunsim_weather PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${WEATHER} FILES ${WEATHER_MODULES})
target_link_libraries(sunsim_weather PUBLIC sunsim_events)

find_package(Threads REQUIRED)
add_executable(sunsim_tests tests.cpp)
target_link_libraries(sunsim_tests PRIVATE sunsim_core sunsim_weather Threads::Threads)
