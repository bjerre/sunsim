# Host tests for the pure layers (sunsim_events + sunsim_core): plain CMake,
# no ESP-IDF. Built with the firmware's C++ settings (no exceptions, no RTTI).
# C++ modules need the Ninja generator (ESP-IDF's export.sh puts one on PATH):
#
#   cmake -S test/host -B build-host -G Ninja && cmake --build build-host && ./build-host/sunsim_tests
cmake_minimum_required(VERSION 3.28)
project(sunsim_host_tests CXX)

set(CMAKE_CXX_STANDARD 26) # the firmware builds as gnu++26 (ESP-IDF 6.1)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clangd (test/host/.clangd)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${ROOT}/cmake/import_std.cmake)

# The module sunsim.core, as the firmware's sunsim_core component builds it.
set(CORE ${ROOT}/components/sunsim_core/src)
file(GLOB_RECURSE CORE_SOURCES ${CORE}/*.cpp)
file(GLOB_RECURSE CORE_MODULES ${CORE}/*.cppm)
add_library(sunsim_core STATIC ${CORE_SOURCES})
target_sources(sunsim_core PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CORE} FILES ${CORE_MODULES})
target_include_directories(sunsim_core PUBLIC
    ${ROOT}/components/eventpp/include
    ${ROOT}/components/sunsim_events/include)
sunsim_import_std(sunsim_core)

find_package(Threads REQUIRED)
add_executable(sunsim_tests tests.cpp)
target_link_libraries(sunsim_tests PRIVATE sunsim_core Threads::Threads)
