add_library(sc_attack_interface INTERFACE)
add_library(sc::attack_interface ALIAS sc_attack_interface)

target_include_directories(sc_attack_interface INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(sc_attack_interface INTERFACE
    sc::runtime
    sc::application
)

if(NOT SC_BUILD_CONTROLLER)
    return()
endif()

add_library(sc_controller STATIC
    CommunicationTask.cpp
    IECCommunicator.cpp
    HmiTask.cpp
    ControlTask.cpp
    SignalProcessingTask.cpp
    MonitoringTask.cpp
)

add_library(sc::controller ALIAS sc_controller)
add_library(tasks ALIAS sc_controller)
add_library(communication ALIAS sc_controller)

target_include_directories(sc_controller
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE ${CPPZMQ_INCLUDE_DIR}
            ${MSGPACK_INCLUDE_DIR}
            ${ZMQ_INCLUDE_DIRS}
)

target_link_libraries(sc_controller
    PUBLIC sc::runtime
           sc::application
           sc::attack_interface
           sc::tcp
           sc::socket_adapters
           sc::iec61850
    PRIVATE ${ZMQ_LIBRARIES}
            sc::project_warnings
)

# HMI messages use only standard-library MsgPack adapters. Avoid requiring the
# optional Boost adapter family merely by including msgpack.hpp.
target_compile_definitions(sc_controller PRIVATE MSGPACK_NO_BOOST)
