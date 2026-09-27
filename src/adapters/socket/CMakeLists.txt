add_library(sc_socket_adapters STATIC
    SocketWrapper.cpp
    SocketWrapperAt.cpp
    SocketWrapperDH.cpp
    SocketWrapperOp.cpp
)

add_library(sc::socket_adapters ALIAS sc_socket_adapters)

target_include_directories(sc_socket_adapters
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
           ${CPPZMQ_INCLUDE_DIR}
           ${ZMQ_INCLUDE_DIRS}
)

target_compile_features(sc_socket_adapters PUBLIC cxx_std_17)

target_link_libraries(sc_socket_adapters
    PUBLIC sc::runtime
           sc::tcp
    PRIVATE ${ZMQ_LIBRARIES}
            sc::project_warnings
)
