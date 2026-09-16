add_library(tasks STATIC
    HmiTask.cpp
    ControlTask.cpp
    SignalProcessingTask.cpp
    MonitoringTask.cpp
)

target_include_directories(tasks
    PUBLIC  ${PROJECT_SOURCE_DIR}           # expose "tasks/XTask.hpp" to consumers
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}     # .cpp files use bare #include "XTask.hpp"
            ${CPPZMQ_INCLUDE_DIR}
            ${MSGPACK_INCLUDE_DIR}
            ${ZMQ_INCLUDE_DIRS}
)

target_link_libraries(tasks
    PUBLIC  common
            sc_application
    PRIVATE ${ZMQ_LIBRARIES}
)

# HMI messages use only standard-library MsgPack adapters. Avoid requiring the
# optional Boost adapter family merely by including msgpack.hpp.
target_compile_definitions(tasks PRIVATE MSGPACK_NO_BOOST)
