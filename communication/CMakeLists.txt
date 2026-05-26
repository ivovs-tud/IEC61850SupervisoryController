
add_library(communication STATIC
    CommunicationTask.cpp
    libiec_wrapper.cpp
    IEC61850Manager.cpp
    socket/SocketWrapper.cpp
    socket/SocketWrapperAt.cpp
    socket/SocketWrapperDH.cpp
    socket/SocketWrapperOp.cpp
)

# ---------------------------------------------------------------------------
# libiec61850 - Platform-specific configuration
# ---------------------------------------------------------------------------
if(WIN32)
    # Windows: Set default path for libiec61850 (update to your Windows path)
    set(LIBIEC61850_DIR "H:/libiec61850" CACHE PATH "Path to libiec61850 source")
    
    add_library(iec61850 SHARED IMPORTED GLOBAL)
    
    # For multi-config generators (Visual Studio), set per-configuration properties
    set_target_properties(iec61850 PROPERTIES
        IMPORTED_LOCATION_Debug "${LIBIEC61850_DIR}/build/src/Debug/iec61850.dll"
        IMPORTED_LOCATION_DEBUG "${LIBIEC61850_DIR}/build/src/Debug/iec61850.dll"
        IMPORTED_IMPLIB_Debug "${LIBIEC61850_DIR}/build/src/Debug/iec61850.lib"
        IMPORTED_IMPLIB_DEBUG "${LIBIEC61850_DIR}/build/src/Debug/iec61850.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBIEC61850_DIR}/src/common/inc;${LIBIEC61850_DIR}/src/iec61850/inc;${LIBIEC61850_DIR}/src/mms/inc;${LIBIEC61850_DIR}/src/goose;${LIBIEC61850_DIR}/src/r_session;${LIBIEC61850_DIR}/hal/inc;${LIBIEC61850_DIR}/src/logging"
    )
    
    # Fallback to Debug if Debug doesn't exist
    if(NOT EXISTS "${LIBIEC61850_DIR}/build/src/Debug/iec61850.dll")
        set_target_properties(iec61850 PROPERTIES
            IMPORTED_LOCATION_DEBUG "${LIBIEC61850_DIR}/build/src/Debug/iec61850.dll"
            IMPORTED_IMPLIB_DEBUG "${LIBIEC61850_DIR}/build/src/Debug/iec61850.lib"
        )
    endif()
else()
    # Linux: Set default path for libiec61850
    set(LIBIEC61850_DIR "/home/ivovs/TestbedProjects/libiec61850" CACHE PATH "Path to libiec61850 source")
    
    add_library(iec61850 SHARED IMPORTED GLOBAL)
    set_target_properties(iec61850 PROPERTIES
        IMPORTED_LOCATION "${LIBIEC61850_DIR}/build/src/libiec61850.so"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBIEC61850_DIR}/src/common/inc;${LIBIEC61850_DIR}/src/iec61850/inc;${LIBIEC61850_DIR}/src/mms/inc;${LIBIEC61850_DIR}/src/goose;${LIBIEC61850_DIR}/src/r_session;${LIBIEC61850_DIR}/hal/inc;${LIBIEC61850_DIR}/src/logging"
    )
endif()

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(communication
        PUBLIC  common
        PRIVATE ${ZMQ_LIBRARIES}
                iec61850
                # Windows uses Threads::Threads from find_package(Threads) instead of pthread
    )
else()
    target_link_libraries(communication
        PUBLIC  common
        PRIVATE ${ZMQ_LIBRARIES}
                iec61850
                pthread
    )
endif()

target_link_directories(communication PRIVATE ${LIBIEC61850_LIBRARY_DIRS})

target_include_directories(communication
    PUBLIC  ${PROJECT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/communication
            ${CPPZMQ_INCLUDE_DIR}
            ${MSGPACK_INCLUDE_DIR}
            ${ZMQ_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/socket
)
