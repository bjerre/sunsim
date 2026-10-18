# Run after each build of sunsim_tests: asks it for its tests' names
# (`--list-test-names-only`) and writes OUT, a CTest file with one test per
# name, each running `sunsim_tests "<name>" --use-colour no`. So CTest, and
# VS Code's Testing panel through CMake Tools, list the tests one by one, as
# gtest_discover_tests does for GoogleTest.
#
#   cmake -D EXE=<sunsim_tests> -D OUT=<file> -P discover_tests.cmake
#
# The names have semicolons in them, which a CMake list would split, so the
# output is walked line by line with string(FIND) instead, and each name goes
# into the file as a bracket argument, which CTest takes whole. ut takes the
# name as a pattern, where . * ! ? and \ mean something, so in the argument
# each of those is `?`, which matches any one character.

execute_process(COMMAND "${EXE}" --list-test-names-only --use-colour no OUTPUT_VARIABLE out RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${EXE} --list-test-names-only failed (${result})")
endif()

set(tests "")
set(count 0)
while(NOT out STREQUAL "")
    string(FIND "${out}" "\n" eol)
    if(eol EQUAL -1)
        set(line "${out}")
        set(out "")
    else()
        string(SUBSTRING "${out}" 0 ${eol} line)
        math(EXPR next "${eol} + 1")
        string(SUBSTRING "${out}" ${next} -1 out)
    endif()
    # ut's summary follows the names: "Suite 'devices': all tests passed ...".
    if(line STREQUAL "" OR line MATCHES "^Suite '[^']*': all tests passed")
        continue()
    endif()
    string(REGEX REPLACE "[.*!?]" "?" pattern "${line}")
    string(REPLACE "\\" "?" pattern "${pattern}")
    string(APPEND tests "add_test([==[${line}]==] [==[${EXE}]==] [==[${pattern}]==] --use-colour no)\n")
    math(EXPR count "${count} + 1")
endwhile()

if(count EQUAL 0)
    message(FATAL_ERROR "${EXE} --list-test-names-only printed no tests")
endif()
file(WRITE "${OUT}" "# Written by discover_tests.cmake from `sunsim_tests --list-test-names-only`: ${count} tests.\n${tests}")
