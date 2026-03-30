
# ZeroMQ
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)
 
# cppzmq (header-only ZMQ C++ bindings)
find_path(CPPZMQ_INCLUDE_DIR
    NAMES zmq.hpp
    PATHS /usr/local/include /usr/include
    REQUIRED
)
 
# msgpack
find_path(MSGPACK_INCLUDE_DIR
    NAMES msgpack.hpp
    PATHS /usr/local/include /usr/include
    REQUIRED
)

add_library(communication STATIC
    CommunicationTask.cpp
    libiec_wrapper.cpp
    IEC61850Manager.cpp
    socket/SocketWrapper.cpp
)

# ---------------------------------------------------------------------------
# libiec61850
# ---------------------------------------------------------------------------
# pkg_check_modules(LIBIEC61850 REQUIRED libiec61850)

# ---------------------------------------------------------------------------
# libiec61850 - link against local build instead of installed version
# ---------------------------------------------------------------------------
set(LIBIEC61850_DIR "/home/ivovs/TestbedProjects/libiec61850" CACHE PATH "Path to libiec61850 source")

add_library(iec61850 SHARED IMPORTED)
set_target_properties(iec61850 PROPERTIES
    IMPORTED_LOCATION "${LIBIEC61850_DIR}/build/src/libiec61850.so"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBIEC61850_DIR}/src/common/inc;${LIBIEC61850_DIR}/src/iec61850/inc;${LIBIEC61850_DIR}/src/mms/inc;${LIBIEC61850_DIR}/src/goose;${LIBIEC61850_DIR}/src/r_session;${LIBIEC61850_DIR}/hal/inc;${LIBIEC61850_DIR}/src/logging"
)

target_link_libraries(communication
    PUBLIC  common
    PRIVATE ${ZMQ_LIBRARIES}
            # ${LIBIEC61850_LIBRARIES}
            iec61850
            pthread
)

target_link_directories(communication PRIVATE ${LIBIEC61850_LIBRARY_DIRS})

target_include_directories(communication
    PUBLIC  ${PROJECT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/communication
            ${CPPZMQ_INCLUDE_DIR}
            ${MSGPACK_INCLUDE_DIR}
            ${ZMQ_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/socket
            # ${LIBIEC61850_INCLUDE_DIRS}
            # ${LIBIEC61850_INCLUDE_DIRS}/libiec61850
)
