# `import std;` / `import std.compat;` with GCC: builds libstdc++'s std
# modules (bits/std.cc, bits/std.compat.cc) as part of `target`, so it and
# every target that links it can import them. Works for the firmware
# (ESP-IDF) and the host tests alike. std.compat is std plus the C names in
# the global namespace (uint8_t, size_t, ...), as this code base uses them.
#
# GCC says where the modules' sources are in libstdc++.modules.json, which
# lives with the multilib the flags select - hence passing them.
function(sunsim_import_std target)
    separate_arguments(flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${flags} -print-file-name=libstdc++.modules.json
                    OUTPUT_VARIABLE manifest OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT IS_ABSOLUTE "${manifest}")
        message(FATAL_ERROR "${CMAKE_CXX_COMPILER} has no libstdc++.modules.json: `import std;` needs GCC >= 15")
    endif()
    cmake_path(GET manifest PARENT_PATH manifest_dir)

    file(READ "${manifest}" json)
    string(JSON count LENGTH "${json}" modules)
    math(EXPR last "${count} - 1")
    set(sources "")
    foreach(i RANGE ${last})
        string(JSON source GET "${json}" modules ${i} source-path)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${manifest_dir}" NORMALIZE)
        list(APPEND sources "${source}")
    endforeach()

    list(GET sources 0 first)
    cmake_path(GET first PARENT_PATH source_dir) # both live in bits/
    target_sources(${target} PUBLIC FILE_SET std TYPE CXX_MODULES BASE_DIRS "${source_dir}" FILES ${sources})
    target_compile_features(${target} PUBLIC cxx_std_26)
endfunction()
