cmake_minimum_required(VERSION 3.15)
project(wind_farm_scada LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectWarnings)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SC_BUILD_TESTS "Build supervisory controller tests" OFF)
option(SC_BUILD_INTEGRATION_TESTS "Build tests that require optional network dependencies" OFF)
option(SC_BUILD_CONTROLLER "Build the IEC 61850 supervisory controller executable" ON)

if(SC_BUILD_INTEGRATION_TESTS AND NOT SC_BUILD_TESTS)
    message(FATAL_ERROR "SC_BUILD_INTEGRATION_TESTS requires SC_BUILD_TESTS=ON")
endif()

# Default to Debug if no build type is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Threading support (works on both Windows and Linux with std::thread)
find_package(Threads REQUIRED)

# Default debugger argument: yaw LUT file placed in project root.
# This file will be passed as the first argument when launching the executable
# from Visual Studio's debugger. You can override at configure time with
# -DDEFAULT_YAW_LUT="/path/to/file.csv".
set(DEFAULT_YAW_LUT "${CMAKE_SOURCE_DIR}/config/yaw_lut.csv" CACHE PATH "Default yaw LUT CSV file passed as first argument to the executable when debugging in Visual Studio")
if(WIN32)
    if(EXISTS "${DEFAULT_YAW_LUT}")
        message(STATUS "Using default yaw LUT for debugger args: ${DEFAULT_YAW_LUT}")
    else()
        message(WARNING "Default yaw LUT not found at ${DEFAULT_YAW_LUT}. Create the file or set -DDEFAULT_YAW_LUT to point to it.")
    endif()
endif()

# Boost – header-only components (circular_buffer, etc.)
# Only the header components are required (e.g. boost::circular_buffer).
# Request the `headers` component so CMake provides the imported target
# `Boost::headers` which can be linked to targets without linking libraries.
find_package(Boost 1.70 REQUIRED)
message(STATUS "Boost ${Boost_VERSION} found at ${Boost_INCLUDE_DIRS}")

# ZeroMQ and cppzmq are needed by the controller and network integration test.
# MsgPack is needed only by the controller's HMI target.
if((SC_BUILD_CONTROLLER OR SC_BUILD_INTEGRATION_TESTS) AND WIN32)
    # Windows with vcpkg: use CONFIG mode for all packages
    find_package(ZeroMQ CONFIG REQUIRED)
    find_package(cppzmq CONFIG REQUIRED)
    find_path(ZMQ_INCLUDE_DIR NAMES zmq.h REQUIRED)
    find_path(CPPZMQ_INCLUDE_DIR NAMES zmq.hpp REQUIRED)
    if(SC_BUILD_CONTROLLER)
        find_path(MSGPACK_INCLUDE_DIR NAMES msgpack.hpp REQUIRED)
    endif()
    
    # Set variables for compatibility with subdirectories
    set(ZMQ_INCLUDE_DIRS ${ZMQ_INCLUDE_DIR})
    set(ZMQ_LIBRARIES libzmq)
elseif(SC_BUILD_CONTROLLER OR SC_BUILD_INTEGRATION_TESTS)
    # Linux: Use pkg-config (or vcpkg if CMAKE_TOOLCHAIN_FILE is set)
    if(DEFINED CMAKE_TOOLCHAIN_FILE)
        # Using vcpkg on Linux
        find_package(ZeroMQ CONFIG REQUIRED)
        find_package(cppzmq CONFIG REQUIRED)
        find_path(ZMQ_INCLUDE_DIR NAMES zmq.h REQUIRED)
        find_path(CPPZMQ_INCLUDE_DIR NAMES zmq.hpp REQUIRED)
        if(SC_BUILD_CONTROLLER)
            find_path(MSGPACK_INCLUDE_DIR NAMES msgpack.hpp REQUIRED)
        endif()
        
        set(ZMQ_INCLUDE_DIRS ${ZMQ_INCLUDE_DIR})
        set(ZMQ_LIBRARIES libzmq)
    else()
        # Traditional Linux package manager installation
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(ZMQ REQUIRED libzmq)
        find_path(CPPZMQ_INCLUDE_DIR NAMES zmq.hpp
            PATHS /usr/local/include /usr/include REQUIRED)
        if(SC_BUILD_CONTROLLER)
            find_path(MSGPACK_INCLUDE_DIR NAMES msgpack.hpp
                PATHS /usr/local/include /usr/include REQUIRED)
        endif()
    endif()
endif()

add_subdirectory(src/runtime)
add_subdirectory(src/application)
add_subdirectory(src/communication)

if(SC_BUILD_CONTROLLER)
    add_subdirectory(src/tasks)
endif()

if(SC_BUILD_CONTROLLER)
    add_subdirectory(apps/wind_farm_scada)
endif()

if(SC_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_subdirectory(tests)
endif()
