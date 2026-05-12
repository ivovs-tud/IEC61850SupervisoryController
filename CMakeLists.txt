cmake_minimum_required(VERSION 3.15)
project(supervisory_controller CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Debug if no build type is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Platform-specific compiler flags
if(MSVC)
    # MSVC compiler options
    add_compile_options(/W4 /permissive-)
else()
    # GCC/Clang compiler options
    add_compile_options(-Wall -Wextra -Wpedantic -fabi-version=18)
endif()

# Threading support (works on both Windows and Linux with std::thread)
find_package(Threads REQUIRED)

# Default debugger argument: yaw LUT file placed in project root.
# This file will be passed as the first argument when launching the executable
# from Visual Studio's debugger. You can override at configure time with
# -DDEFAULT_YAW_LUT="/path/to/file.csv".
set(DEFAULT_YAW_LUT "${CMAKE_SOURCE_DIR}/yaw_lut.csv" CACHE PATH "Default yaw LUT CSV file passed as first argument to the executable when debugging in Visual Studio")
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

# ZeroMQ + cppzmq + msgpack
if(WIN32)
    # Windows with vcpkg: use CONFIG mode for all packages
    find_package(ZeroMQ CONFIG REQUIRED)
    find_package(cppzmq CONFIG REQUIRED)
    # msgpack is header-only, just find the include directory
    find_path(MSGPACK_INCLUDE_DIR NAMES msgpack.hpp REQUIRED)
    
    # Set variables for compatibility with subdirectories
    set(ZMQ_INCLUDE_DIRS ${ZeroMQ_INCLUDE_DIR})
    set(ZMQ_LIBRARIES libzmq)
    set(CPPZMQ_INCLUDE_DIR ${cppzmq_INCLUDE_DIR})
else()
    # Linux: Use pkg-config (or vcpkg if CMAKE_TOOLCHAIN_FILE is set)
    if(DEFINED CMAKE_TOOLCHAIN_FILE)
        # Using vcpkg on Linux
        find_package(ZeroMQ CONFIG REQUIRED)
        find_package(cppzmq CONFIG REQUIRED)
        find_path(MSGPACK_INCLUDE_DIR NAMES msgpack.hpp REQUIRED)
        
        set(ZMQ_INCLUDE_DIRS ${ZeroMQ_INCLUDE_DIR})
        set(ZMQ_LIBRARIES libzmq)
        set(CPPZMQ_INCLUDE_DIR ${cppzmq_INCLUDE_DIR})
    else()
        # Traditional Linux package manager installation
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(ZMQ REQUIRED libzmq)
        find_path(CPPZMQ_INCLUDE_DIR NAMES zmq.hpp
            PATHS /usr/local/include /usr/include REQUIRED)
        find_path(MSGPACK_INCLUDE_DIR NAMES msgpack.hpp
            PATHS /usr/local/include /usr/include REQUIRED)
    endif()
endif()

add_subdirectory(common)
add_subdirectory(tasks)
add_subdirectory(communication)

add_executable(supervisory_controller main.cpp)

target_link_libraries(supervisory_controller PRIVATE
    tasks
    communication
    Threads::Threads
    Boost::headers
)

# Windows: Copy required DLLs to executable directory for runtime
if(WIN32)
    # Copy libiec61850 DLL
    set(LIBIEC61850_DIR "H:/libiec61850" CACHE PATH "Path to libiec61850 source")
    add_custom_command(TARGET supervisory_controller POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LIBIEC61850_DIR}/build/src/Debug/iec61850.dll"
            $<TARGET_FILE_DIR:supervisory_controller>
        COMMENT "Copying iec61850.dll to output directory"
    )
    
    # Copy ZeroMQ DLL (from vcpkg)
    if(TARGET libzmq)
        add_custom_command(TARGET supervisory_controller POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:libzmq>
                $<TARGET_FILE_DIR:supervisory_controller>
            COMMENT "Copying libzmq.dll to output directory"
        )
    endif()
    
    # Set debugging command arguments for Visual Studio
    # Edit these arguments as needed
    # Visual Studio is a multi-config generator. Set both the generic and
    # per-configuration debugger argument properties so the value is picked up
    # regardless of the active configuration in the IDE.
    set_target_properties(supervisory_controller PROPERTIES
        VS_DEBUGGER_COMMAND_ARGUMENTS "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_DEBUG "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_RELEASE "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_RELWITHDEBINFO "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_MINSIZEREL "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}"
    )
    
    # Optional: Add a custom target to run with arguments
    add_custom_target(run
        COMMAND supervisory_controller "${DEFAULT_YAW_LUT}"
        DEPENDS supervisory_controller
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running supervisory_controller with arguments"
    )
endif()
