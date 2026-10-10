# `import std;` with GCC: builds libstdc++'s std module (bits/std.cc) as part
# of `target`, so it and every target that links it can import std. Works for
# the firmware (ESP-IDF) and the host tests alike.
#
# GCC says where the module's source is in libstdc++.modules.json, which
# lives with the multilib the flags select - hence passing them.
function(sunsim_import_std target)
    separate_arguments(flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${flags} -print-file-name=libstdc++.modules.json
                    OUTPUT_VARIABLE manifest OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT IS_ABSOLUTE "${manifest}")
        message(FATAL_ERROR "${CMAKE_CXX_COMPILER} has no libstdc++.modules.json: `import std;` needs GCC >= 15")
    endif()

    file(READ "${manifest}" json)
    string(JSON count LENGTH "${json}" modules)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        string(JSON name GET "${json}" modules ${i} logical-name)
        if(name STREQUAL "std")
            string(JSON source GET "${json}" modules ${i} source-path)
        endif()
    endforeach()

    cmake_path(GET manifest PARENT_PATH manifest_dir)
    cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${manifest_dir}" NORMALIZE)
    cmake_path(GET source PARENT_PATH source_dir)
    target_sources(${target} PUBLIC FILE_SET std TYPE CXX_MODULES BASE_DIRS "${source_dir}" FILES "${source}")
    target_compile_features(${target} PUBLIC cxx_std_26)
endfunction()
