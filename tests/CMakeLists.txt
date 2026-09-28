add_executable(sc_protocol_layout_tests
    unit/ProtocolLayoutTests.cpp
)

target_link_libraries(sc_protocol_layout_tests PRIVATE
    Catch2::Catch2WithMain
    sc_runtime
)

include(Catch)
catch_discover_tests(
    sc_protocol_layout_tests
    TEST_PREFIX "sc.protocol_layout."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_attack_protocol_tests
    unit/AttackProtocolTests.cpp
)

target_link_libraries(sc_attack_protocol_tests PRIVATE
    Catch2::Catch2WithMain
    sc_runtime
)

catch_discover_tests(
    sc_attack_protocol_tests
    TEST_PREFIX "sc.attack_protocol."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_attack_interface_tests
    unit/AttackInterfaceTests.cpp
)

target_include_directories(sc_attack_interface_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sc_attack_interface_tests PRIVATE
    Catch2::Catch2WithMain
    sc_attack
)

catch_discover_tests(
    sc_attack_interface_tests
    TEST_PREFIX "sc.attack_interface."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_attack_session_manager_tests
    unit/AttackSessionManagerTests.cpp
)

target_include_directories(sc_attack_session_manager_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sc_attack_session_manager_tests PRIVATE
    Catch2::Catch2WithMain
    sc_attack
)

catch_discover_tests(
    sc_attack_session_manager_tests
    TEST_PREFIX "sc.attack_session_manager."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_yaw_lut_tests
    unit/YawLutTests.cpp
)

target_include_directories(sc_yaw_lut_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sc_yaw_lut_tests PRIVATE
    Catch2::Catch2WithMain
    sc_application
)

catch_discover_tests(
    sc_yaw_lut_tests
    TEST_PREFIX "sc.yaw_lut."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_control_calculation_tests
    unit/ControlCalculationTests.cpp
)

target_include_directories(sc_control_calculation_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sc_control_calculation_tests PRIVATE
    Catch2::Catch2WithMain
    sc_application
)

catch_discover_tests(
    sc_control_calculation_tests
    TEST_PREFIX "sc.control_calculation."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_signal_processing_tests
    unit/SignalProcessingTests.cpp
)

target_link_libraries(sc_signal_processing_tests PRIVATE
    Catch2::Catch2WithMain
    sc_application
)

catch_discover_tests(
    sc_signal_processing_tests
    TEST_PREFIX "sc.signal_processing."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_monitoring_tests
    unit/MonitoringTests.cpp
)

target_link_libraries(sc_monitoring_tests PRIVATE
    Catch2::Catch2WithMain
    sc_application
)

catch_discover_tests(
    sc_monitoring_tests
    TEST_PREFIX "sc.monitoring."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_runtime_config_tests
    unit/RuntimeConfigTests.cpp
)

target_include_directories(sc_runtime_config_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(sc_runtime_config_tests PRIVATE
    SC_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(sc_runtime_config_tests PRIVATE
    Catch2::Catch2WithMain
    sc_runtime
    sc_application
)

catch_discover_tests(
    sc_runtime_config_tests
    TEST_PREFIX "sc.runtime_config."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_periodic_task_tests
    unit/PeriodicTaskTests.cpp
)

target_link_libraries(sc_periodic_task_tests PRIVATE
    Catch2::Catch2WithMain
    sc_runtime
)

catch_discover_tests(
    sc_periodic_task_tests
    TEST_PREFIX "sc.periodic_task."
    DISCOVERY_MODE PRE_TEST
)

if(TARGET sc_iec61850)
    add_executable(sc_iec_lifecycle_tests
        unit/IecLifecycleTests.cpp
    )

    target_include_directories(sc_iec_lifecycle_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/communication/iec61850
    )

    target_link_libraries(sc_iec_lifecycle_tests PRIVATE
        Catch2::Catch2WithMain
        sc_iec61850
    )

    if(WIN32)
        add_custom_command(TARGET sc_iec_lifecycle_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:iec61850>
                    $<TARGET_FILE_DIR:sc_iec_lifecycle_tests>
        )
        if(TARGET libzmq)
            add_custom_command(TARGET sc_iec_lifecycle_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:libzmq>
                        $<TARGET_FILE_DIR:sc_iec_lifecycle_tests>
            )
        endif()
    endif()

    catch_discover_tests(
        sc_iec_lifecycle_tests
        TEST_PREFIX "sc.iec_lifecycle."
        DISCOVERY_MODE PRE_TEST
    )
endif()

add_test(
    NAME sc.python_attack_protocol
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_CURRENT_SOURCE_DIR}/protocol
            -p test_attack_protocol.py
)

set_tests_properties(sc.python_attack_protocol PROPERTIES
    ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME sc.python_attack_client
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_CURRENT_SOURCE_DIR}/protocol
            -p test_attack_client.py
)

set_tests_properties(sc.python_attack_client PROPERTIES
    ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;PYTHONDONTWRITEBYTECODE=1"
)

if(SC_BUILD_INTEGRATION_TESTS)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import zmq"
        RESULT_VARIABLE SC_PYZMQ_IMPORT_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT SC_PYZMQ_IMPORT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "SC_BUILD_INTEGRATION_TESTS requires pyzmq. "
            "Install the Python package with: "
            "python -m pip install -e .")
    endif()

    add_executable(sc_attack_loopback_server
        integration/AttackLoopbackServer.cpp
    )

    target_link_libraries(sc_attack_loopback_server PRIVATE
        sc_runtime
        sc_attack
        sc_attack_zmq
        Threads::Threads
    )

    if(WIN32)
        if(TARGET libzmq)
            add_custom_command(TARGET sc_attack_loopback_server POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:libzmq>
                        $<TARGET_FILE_DIR:sc_attack_loopback_server>
            )
        endif()
    endif()

    add_test(
        NAME sc.integration.attack_loopback
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_attack_loopback.py
                $<TARGET_FILE:sc_attack_loopback_server>
    )
    set_tests_properties(sc.integration.attack_loopback PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30
    )

    add_executable(sc_raw_attack_loopback_server
        integration/RawAttackLoopbackServer.cpp
    )

    target_link_libraries(sc_raw_attack_loopback_server PRIVATE
        sc_runtime
        sc_attack
        sc_attack_tcp
        Threads::Threads
    )

    add_test(
        NAME sc.integration.raw_attack_loopback
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_raw_attack_loopback.py
                $<TARGET_FILE:sc_raw_attack_loopback_server>
    )
    set_tests_properties(sc.integration.raw_attack_loopback PROPERTIES
        ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30
    )

    add_executable(sc_data_historian_tcp_tests
        integration/DataHistorianTcpTests.cpp
    )

    target_link_libraries(sc_data_historian_tcp_tests PRIVATE
        sc_runtime
        sc_data_historian_server
        sc_tcp_server
        Threads::Threads
    )

    target_include_directories(sc_data_historian_tcp_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/communication/network
    )

    if(WIN32)
        if(TARGET libzmq)
            add_custom_command(TARGET sc_data_historian_tcp_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:libzmq>
                        $<TARGET_FILE_DIR:sc_data_historian_tcp_tests>
            )
        endif()
    endif()

    add_test(NAME sc.integration.data_historian_tcp COMMAND sc_data_historian_tcp_tests)
    set_tests_properties(sc.integration.data_historian_tcp PROPERTIES TIMEOUT 15)
endif()
