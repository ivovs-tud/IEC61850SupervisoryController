add_library(sc_attack STATIC
    attack/AttackInterface.cpp
    attack/AttackSessionManager.cpp
)

target_include_directories(sc_attack PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(sc_attack PUBLIC cxx_std_17)
target_link_libraries(sc_attack PUBLIC sc_runtime PRIVATE sc::project_warnings)

if(SC_BUILD_CONTROLLER OR SC_BUILD_INTEGRATION_TESTS)
    add_library(sc_tcp_server STATIC network/TcpServer.cpp)
    target_include_directories(sc_tcp_server
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network
    )
    target_compile_features(sc_tcp_server PUBLIC cxx_std_17)
    target_link_libraries(sc_tcp_server PUBLIC sc_runtime PRIVATE sc::project_warnings)
    if(WIN32)
        target_link_libraries(sc_tcp_server PUBLIC ws2_32)
    endif()

    add_library(sc_attack_tcp STATIC attack/AttackChannelTCP.cpp)
    target_include_directories(sc_attack_tcp PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_features(sc_attack_tcp PUBLIC cxx_std_17)
    target_link_libraries(sc_attack_tcp
        PUBLIC sc_attack sc_tcp_server sc_runtime
        PRIVATE sc::project_warnings
    )

    add_library(sc_attack_zmq STATIC attack/AttackChannelZMQ.cpp)
    target_include_directories(sc_attack_zmq
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               ${CPPZMQ_INCLUDE_DIR}
               ${ZMQ_INCLUDE_DIRS}
    )
    target_compile_features(sc_attack_zmq PUBLIC cxx_std_17)
    target_link_libraries(sc_attack_zmq
        PUBLIC sc_attack sc_runtime
        PRIVATE ${ZMQ_LIBRARIES} sc::project_warnings
    )

    add_library(sc_operator_server STATIC operator/OperatorServer.cpp)
    target_include_directories(sc_operator_server
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               ${CPPZMQ_INCLUDE_DIR}
               ${ZMQ_INCLUDE_DIRS}
    )
    target_compile_features(sc_operator_server PUBLIC cxx_std_17)
    target_link_libraries(sc_operator_server
        PUBLIC sc_runtime
        PRIVATE ${ZMQ_LIBRARIES} sc::project_warnings
    )

    add_library(sc_data_historian_server STATIC data_historian/DataHistorianServer.cpp)
    target_include_directories(sc_data_historian_server PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_features(sc_data_historian_server PUBLIC cxx_std_17)
    target_link_libraries(sc_data_historian_server
        PUBLIC sc_runtime sc_tcp_server
        PRIVATE sc::project_warnings
    )
endif()

if(SC_BUILD_CONTROLLER)
    include(LibIec61850)
    sc_configure_libiec61850()

    add_library(sc_hmi STATIC hmi/HmiInterface.cpp)
    target_include_directories(sc_hmi
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               ${CPPZMQ_INCLUDE_DIR}
               ${ZMQ_INCLUDE_DIRS}
        PRIVATE ${MSGPACK_INCLUDE_DIR}
    )
    target_compile_definitions(sc_hmi PRIVATE MSGPACK_NO_BOOST)
    target_compile_features(sc_hmi PUBLIC cxx_std_17)
    target_link_libraries(sc_hmi PUBLIC sc_runtime PRIVATE ${ZMQ_LIBRARIES} sc::project_warnings)

    add_library(sc_iec61850 STATIC
        iec61850/IEC61850Manager.cpp
        iec61850/IECCommunicator.cpp
        iec61850/LibIecGooseReceiver.cpp
    )
    target_include_directories(sc_iec61850
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/iec61850
    )
    target_compile_features(sc_iec61850 PUBLIC cxx_std_17)
    target_link_libraries(sc_iec61850
        PUBLIC sc_attack sc_runtime
        PRIVATE libiec61850::iec61850 Threads::Threads sc::project_warnings
    )

    add_library(sc_communication STATIC CommunicationOrchestrator.cpp)
    target_include_directories(sc_communication PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_features(sc_communication PUBLIC cxx_std_17)
    target_link_libraries(sc_communication
        PUBLIC sc_attack
               sc_attack_tcp
               sc_attack_zmq
               sc_data_historian_server
               sc_hmi
               sc_iec61850
               sc_operator_server
               sc_runtime
        PRIVATE sc::project_warnings
    )
endif()
