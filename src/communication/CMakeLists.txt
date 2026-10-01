add_library(sc_attack STATIC
    attack/AttackInterface.cpp
    attack/AttackSessionManager.cpp
)
add_library(sc::attack ALIAS sc_attack)

target_include_directories(sc_attack PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(sc_attack PUBLIC cxx_std_17)
target_link_libraries(sc_attack PUBLIC sc::runtime PRIVATE sc::project_warnings)

if(SC_BUILD_CONTROLLER OR SC_BUILD_INTEGRATION_TESTS)
    add_library(sc_tcp_server STATIC network/TcpServer.cpp)
    add_library(sc::tcp_server ALIAS sc_tcp_server)
    target_include_directories(sc_tcp_server
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network
    )
    target_compile_features(sc_tcp_server PUBLIC cxx_std_17)
    target_link_libraries(sc_tcp_server PUBLIC sc::runtime PRIVATE sc::project_warnings)
    if(WIN32)
        target_link_libraries(sc_tcp_server PUBLIC ws2_32)
    endif()

    add_library(sc_attack_tcp STATIC attack/AttackChannelTCP.cpp)
    add_library(sc::attack_tcp ALIAS sc_attack_tcp)
    target_include_directories(sc_attack_tcp PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(sc_attack_tcp PUBLIC cxx_std_17)
    target_link_libraries(sc_attack_tcp
        PUBLIC sc::attack sc::tcp_server sc::runtime
        PRIVATE sc::project_warnings
    )

    add_library(sc_attack_zmq STATIC attack/AttackChannelZMQ.cpp)
    add_library(sc::attack_zmq ALIAS sc_attack_zmq)
    target_include_directories(sc_attack_zmq
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/attack>
               $<BUILD_INTERFACE:${CPPZMQ_INCLUDE_DIR}>
               $<BUILD_INTERFACE:${ZMQ_INCLUDE_DIRS}>
    )
    target_compile_features(sc_attack_zmq PUBLIC cxx_std_17)
    target_link_libraries(sc_attack_zmq
        PUBLIC sc::attack sc::runtime
        PRIVATE ${ZMQ_LIBRARIES} sc::project_warnings
    )

    add_library(sc_operator_server STATIC operator/OperatorServer.cpp)
    add_library(sc::operator_server ALIAS sc_operator_server)
    target_include_directories(sc_operator_server
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/operator>
               $<BUILD_INTERFACE:${CPPZMQ_INCLUDE_DIR}>
               $<BUILD_INTERFACE:${ZMQ_INCLUDE_DIRS}>
    )
    target_compile_features(sc_operator_server PUBLIC cxx_std_17)
    target_link_libraries(sc_operator_server
        PUBLIC sc::runtime
        PRIVATE ${ZMQ_LIBRARIES} sc::project_warnings
    )

    add_library(sc_data_historian_server STATIC data_historian/DataHistorianServer.cpp)
    add_library(sc::data_historian_server ALIAS sc_data_historian_server)
    target_include_directories(sc_data_historian_server PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(sc_data_historian_server PUBLIC cxx_std_17)
    target_link_libraries(sc_data_historian_server
        PUBLIC sc::runtime sc::tcp_server
        PRIVATE sc::project_warnings
    )
endif()

if(SC_BUILD_CONTROLLER)
    include(LibIec61850)
    sc_configure_libiec61850()

    add_library(sc_hmi STATIC hmi/HmiInterface.cpp)
    add_library(sc::hmi ALIAS sc_hmi)
    target_include_directories(sc_hmi
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:include>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/hmi>
               $<BUILD_INTERFACE:${CPPZMQ_INCLUDE_DIR}>
               $<BUILD_INTERFACE:${ZMQ_INCLUDE_DIRS}>
        PRIVATE ${MSGPACK_INCLUDE_DIR}
    )
    target_compile_definitions(sc_hmi PRIVATE MSGPACK_NO_BOOST)
    target_compile_features(sc_hmi PUBLIC cxx_std_17)
    target_link_libraries(sc_hmi PUBLIC sc::runtime PRIVATE ${ZMQ_LIBRARIES} sc::project_warnings)

    add_library(sc_iec61850 STATIC
        iec61850/IEC61850Manager.cpp
        iec61850/IECCommunicator.cpp
        iec61850/LibIecGooseReceiver.cpp
    )
    add_library(sc::iec61850 ALIAS sc_iec61850)
    target_include_directories(sc_iec61850
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/iec61850
    )
    target_compile_features(sc_iec61850 PUBLIC cxx_std_17)
    target_link_libraries(sc_iec61850
        PUBLIC sc::attack sc::runtime
        PRIVATE libiec61850::iec61850 Threads::Threads sc::project_warnings
    )

    add_library(sc_communication STATIC
        CommunicationConfig.cpp
        CommunicationOrchestrator.cpp
    )
    add_library(sc::communication ALIAS sc_communication)
    target_include_directories(sc_communication PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(sc_communication PUBLIC cxx_std_17)
    target_link_libraries(sc_communication
        PUBLIC sc::attack
               sc::attack_tcp
               sc::attack_zmq
               sc::data_historian_server
               sc::hmi
               sc::iec61850
               sc::operator_server
               sc::runtime
        PRIVATE sc::project_warnings
    )
endif()
