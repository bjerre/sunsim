# Host tests for the pure layers (sunsim_events, sunsim_core, sunsim_weather) and
# the simulator (sim/): plain CMake,
# no ESP-IDF. Built with the firmware's C++ settings (no exceptions, no RTTI).
# C++ modules need the Ninja generator (ESP-IDF's export.sh puts one on PATH).
# The test framework, boost-ext/ut, comes from vcpkg (vcpkg.json), found
# through VCPKG_ROOT (default ~/vcpkg). Each test is a CTest test too, so
# `ctest` and VS Code's Testing panel (CMake Tools, CMakePresets.json) list
# them one by one:
#
#   cmake --preset host && cmake --build --preset host && ctest --preset host
#   ./build-host/sunsim_tests ["<test name>"]
cmake_minimum_required(VERSION 3.28)
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(vcpkg $ENV{VCPKG_ROOT})
    if(NOT vcpkg)
        set(vcpkg $ENV{HOME}/vcpkg)
    endif()
    if(NOT EXISTS ${vcpkg}/scripts/buildsystems/vcpkg.cmake)
        message(FATAL_ERROR "No vcpkg at ${vcpkg}: set VCPKG_ROOT to a vcpkg checkout (the host tests get boost-ext/ut from it)")
    endif()
    set(CMAKE_TOOLCHAIN_FILE ${vcpkg}/scripts/buildsystems/vcpkg.cmake)
endif()
project(sunsim_host_tests CXX)

set(CMAKE_CXX_STANDARD 26) # the firmware builds as gnu++26 (ESP-IDF 6.1)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${ROOT}/cmake/import_std.cmake)

# The modules sunsim.ev, sunsim.core, sunsim.weather and sunsim.presentation,
# as the firmware's components build them.
set(EVENTS ${ROOT}/components/sunsim_events)
file(GLOB EVENTS_MODULES CONFIGURE_DEPENDS ${EVENTS}/src/*.cppm)
add_library(sunsim_events STATIC)
target_sources(sunsim_events PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${EVENTS}/src FILES ${EVENTS_MODULES})
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

set(PRESENTATION ${ROOT}/components/sunsim_presentation/src)
file(GLOB PRESENTATION_MODULES CONFIGURE_DEPENDS ${PRESENTATION}/*.cppm)
add_library(sunsim_presentation STATIC)
target_sources(sunsim_presentation PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${PRESENTATION}
                                          FILES ${PRESENTATION_MODULES})
target_link_libraries(sunsim_presentation PUBLIC sunsim_events)

# sunsim on simulated hardware (sunsim.sim): virtual time, clock and Zigbee
# network, for whole-app scenarios.
set(SIM ${ROOT}/sim/src)
file(GLOB SIM_MODULES CONFIGURE_DEPENDS ${SIM}/*.cppm)
add_library(sunsim_sim STATIC)
target_sources(sunsim_sim PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${SIM} FILES ${SIM_MODULES})
# SimClock's definitions are in a module implementation unit (sim_clock.cppm says why).
target_sources(sunsim_sim PRIVATE ${SIM}/sim_clock.cpp)
target_link_libraries(sunsim_sim PUBLIC sunsim_core)

# boost-ext/ut's module, boost.ut. vcpkg installs ut.cppm beside ut.hpp but
# doesn't build it; built here against the same std module as the rest.
# (Copied into the build tree for clangd, as import_std.cmake does; ut.cppm
# includes "ut.hpp", so that comes too.)
find_package(ut CONFIG REQUIRED)
get_target_property(UT_INCLUDE Boost::ut INTERFACE_INCLUDE_DIRECTORIES)
foreach(file ut.cppm ut.hpp)
    configure_file(${UT_INCLUDE}/boost/${file} ${CMAKE_CURRENT_BINARY_DIR}/ut/${file} COPYONLY)
endforeach()
add_library(boost_ut STATIC)
target_sources(boost_ut PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/ut FILES
                                                    ${CMAKE_CURRENT_BINARY_DIR}/ut/ut.cppm)
target_link_libraries(boost_ut PUBLIC sunsim_events)

find_package(Threads REQUIRED)
# One file per area (devices, modes, daylight, weather, events, sends, sim), sharing the
# module sunsim.test (harness.cppm).
add_executable(sunsim_tests main.cpp devices_tests.cpp modes_tests.cpp daylight_tests.cpp weather_tests.cpp
                            events_tests.cpp presentation_tests.cpp sends_tests.cpp sim_tests.cpp)
target_sources(sunsim_tests PRIVATE FILE_SET CXX_MODULES FILES harness.cppm)
target_link_libraries(sunsim_tests PRIVATE sunsim_core sunsim_sim sunsim_weather sunsim_presentation boost_ut
                                           Threads::Threads)

# A CTest test per ut test, named as it is: after each build,
# discover_tests.cmake asks sunsim_tests for the names and writes the CTest
# file that's included here.
enable_testing()
set(DISCOVERED ${CMAKE_CURRENT_BINARY_DIR}/sunsim_tests_discovered.cmake)
add_custom_command(TARGET sunsim_tests POST_BUILD BYPRODUCTS ${DISCOVERED}
                   COMMAND ${CMAKE_COMMAND} -D EXE=$<TARGET_FILE:sunsim_tests> -D OUT=${DISCOVERED} -P
                           ${CMAKE_CURRENT_SOURCE_DIR}/discover_tests.cmake
                   COMMENT "Finding the tests in sunsim_tests")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${DISCOVERED})
