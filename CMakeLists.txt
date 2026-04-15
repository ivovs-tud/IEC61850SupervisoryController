cmake_minimum_required(VERSION 3.15)
project(supervisory_controller CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Debug if no build type is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -fabi-version=18)

find_package(Threads REQUIRED)

# Boost – header-only components (circular_buffer, etc.)
find_package(Boost 1.70 REQUIRED)
message(STATUS "Boost ${Boost_VERSION} found at ${Boost_INCLUDE_DIRS}")

# ZeroMQ + cppzmq + msgpack – shared by tasks and communication
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)
find_path(CPPZMQ_INCLUDE_DIR NAMES zmq.hpp
    PATHS /usr/local/include /usr/include REQUIRED)
find_path(MSGPACK_INCLUDE_DIR NAMES msgpack.hpp
    PATHS /usr/local/include /usr/include REQUIRED)

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
