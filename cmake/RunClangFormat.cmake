cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED SC_FORMAT_MODE)
    set(SC_FORMAT_MODE check)
endif()

if(NOT SC_FORMAT_MODE STREQUAL "check" AND NOT SC_FORMAT_MODE STREQUAL "format")
    message(FATAL_ERROR "SC_FORMAT_MODE must be 'check' or 'format'")
endif()

if(NOT DEFINED SC_PROJECT_ROOT)
    get_filename_component(SC_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT SC_CLANG_FORMAT_EXECUTABLE)
    find_program(SC_CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format-18)
endif()

if(NOT SC_CLANG_FORMAT_EXECUTABLE)
    message(FATAL_ERROR "clang-format 18.1.8 was not found. Install it with: python -m pip install clang-format==18.1.8")
endif()

execute_process(
    COMMAND "${SC_CLANG_FORMAT_EXECUTABLE}" --version
    OUTPUT_VARIABLE clangFormatVersion
    ERROR_VARIABLE clangFormatVersionError
    RESULT_VARIABLE clangFormatVersionResult
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT clangFormatVersionResult EQUAL 0)
    message(FATAL_ERROR "Could not run ${SC_CLANG_FORMAT_EXECUTABLE}: ${clangFormatVersionError}")
endif()

if(NOT clangFormatVersion MATCHES "clang-format version 18\\.1\\.8")
    message(FATAL_ERROR "Wind Farm SCADA requires clang-format 18.1.8, but found: ${clangFormatVersion}")
endif()

file(GLOB_RECURSE scFormatFiles LIST_DIRECTORIES false
    "${SC_PROJECT_ROOT}/apps/*.cpp"
    "${SC_PROJECT_ROOT}/apps/*.h"
    "${SC_PROJECT_ROOT}/apps/*.hpp"
    "${SC_PROJECT_ROOT}/include/*.cpp"
    "${SC_PROJECT_ROOT}/include/*.h"
    "${SC_PROJECT_ROOT}/include/*.hpp"
    "${SC_PROJECT_ROOT}/src/*.cpp"
    "${SC_PROJECT_ROOT}/src/*.h"
    "${SC_PROJECT_ROOT}/src/*.hpp"
    "${SC_PROJECT_ROOT}/tests/*.cpp"
    "${SC_PROJECT_ROOT}/tests/*.h"
    "${SC_PROJECT_ROOT}/tests/*.hpp"
)
list(SORT scFormatFiles)

set(formatFailures "")
foreach(sourceFile IN LISTS scFormatFiles)
    if(SC_FORMAT_MODE STREQUAL "format")
        execute_process(
            COMMAND "${SC_CLANG_FORMAT_EXECUTABLE}" -i --style=file "${sourceFile}"
            RESULT_VARIABLE formatResult
            ERROR_VARIABLE formatError
        )
    else()
        execute_process(
            COMMAND "${SC_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror --style=file "${sourceFile}"
            RESULT_VARIABLE formatResult
            OUTPUT_VARIABLE formatOutput
            ERROR_VARIABLE formatError
        )
    endif()

    if(NOT formatResult EQUAL 0)
        string(APPEND formatFailures "\n${sourceFile}\n${formatOutput}${formatError}")
    endif()
endforeach()

if(formatFailures)
    message(FATAL_ERROR "clang-format failed:${formatFailures}")
endif()

list(LENGTH scFormatFiles formattedFileCount)
message(STATUS "clang-format ${SC_FORMAT_MODE} passed for ${formattedFileCount} files")
