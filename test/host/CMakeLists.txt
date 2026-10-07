# Host tests for the pure layers (sunsim_events + sunsim_core): plain CMake,
# no ESP-IDF. Built with the firmware's C++ settings (no exceptions, no RTTI).
#
#   cmake -S test/host -B build-host && cmake --build build-host && ./build-host/sunsim_tests
cmake_minimum_required(VERSION 3.16)
project(sunsim_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
file(GLOB CORE_SOURCES ${ROOT}/components/sunsim_core/src/*.cpp)

find_package(Threads REQUIRED)
add_executable(sunsim_tests tests.cpp ${CORE_SOURCES})
target_include_directories(sunsim_tests PRIVATE
    ${ROOT}/components/eventpp/include
    ${ROOT}/components/sunsim_events/include
    ${ROOT}/components/sunsim_core/include)
target_link_libraries(sunsim_tests PRIVATE Threads::Threads)
