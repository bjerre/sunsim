# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.28)

# C++ modules (sunsim.core, ...): scan sources for `import` so they build in
# order. Needs the Ninja generator (idf.py's default) and GCC >= 15.
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(sunsim)
